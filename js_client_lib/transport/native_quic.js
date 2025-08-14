// Minimal Node FFI loader for libwwatp_quic (C facade).
// This is a POC for driving the C stub or future real QuicConnector wiring.
// It is Node-only and guarded to fail gracefully when ffi modules or the .so are missing.

import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import { createRequire } from 'module';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const repoRoot = path.resolve(__dirname, '..', '..');

function resolveLibPath(overridePath) {
  if (overridePath && fs.existsSync(overridePath)) return path.resolve(overridePath);
  // default build output under repo root
  const candidates = [
    path.join(repoRoot, 'build', 'libwwatp_quic.so'), // Linux
    path.join(repoRoot, 'build', 'wwatp_quic.dll'),   // Windows
    path.join(repoRoot, 'build', 'libwwatp_quic.dylib'), // macOS
  ];
  for (const p of candidates) {
    if (fs.existsSync(p)) return p;
  }
  return null;
}

export function tryLoadNativeQuic(libPathOverride = null) {
  // Only in Node
  if (typeof process === 'undefined' || !process.versions?.node) return null;
  const require = createRequire(import.meta.url);
  let ffi, ref, Struct;
  try {
    ffi = require('ffi-napi');
    ref = require('ref-napi');
    Struct = require('ref-struct-napi');
  } catch (_e) {
    return null; // modules not installed
  }

  const libPath = resolveLibPath(libPathOverride);
  if (!libPath) return null;

  // Define C types
  const voidPtr = ref.refType(ref.types.void);
  const size_t = ref.types.size_t;

  const SessionOpts = Struct({
    url: ref.types.CString,
    authority: ref.types.CString,
    cert_file: ref.types.CString,
    key_file: ref.types.CString,
    ca_file: ref.types.CString,
    insecure_skip_verify: ref.types.int,
    timeout_ms: ref.types.uint32,
  });

  const lib = ffi.Library(libPath, {
    wwatp_quic_create_session: [voidPtr, [ref.refType(SessionOpts)]],
    wwatp_quic_close_session: ['void', [voidPtr]],
    wwatp_quic_open_bidi_stream: [voidPtr, [voidPtr, 'string']],
    wwatp_quic_stream_write: ['int64', [voidPtr, 'pointer', size_t, 'int']],
    wwatp_quic_stream_read: ['int64', [voidPtr, 'pointer', size_t, 'uint32']],
    wwatp_quic_stream_close: ['void', [voidPtr]],
    wwatp_quic_last_error: ['string', []],
  });

  function makeOpts(opts = {}) {
    const so = new SessionOpts({
      url: opts.url || null,
      authority: opts.authority || null,
      cert_file: opts.cert_file || null,
      key_file: opts.key_file || null,
      ca_file: opts.ca_file || null,
      insecure_skip_verify: opts.insecure_skip_verify ? 1 : 0,
      timeout_ms: opts.timeout_ms >>> 0 || 0,
    });
    return so;
  }

  function lastError() {
    try { return lib.wwatp_quic_last_error(); } catch { return 'unknown'; }
  }

  function createSession(opts) {
    const o = makeOpts(opts);
    const ptr = lib.wwatp_quic_create_session(o.ref());
    if (ptr.isNull()) throw new Error('create_session failed: ' + lastError());
    return ptr;
  }

  function closeSession(sessionPtr) {
    if (sessionPtr && !sessionPtr.isNull()) lib.wwatp_quic_close_session(sessionPtr);
  }

  function openBidiStream(sessionPtr, pathStr) {
    const st = lib.wwatp_quic_open_bidi_stream(sessionPtr, pathStr);
    if (st.isNull()) throw new Error('open_bidi_stream failed: ' + lastError());
    return st;
  }

  function write(streamPtr, data, endStream = true) {
    const buf = Buffer.isBuffer(data) ? data : Buffer.from(data instanceof Uint8Array ? data : new Uint8Array());
    const rc = lib.wwatp_quic_stream_write(streamPtr, buf, buf.length, endStream ? 1 : 0);
    return Number(rc);
  }

  function read(streamPtr, maxLen = 65536, timeoutMs = 0) {
    const buf = Buffer.allocUnsafe(maxLen);
    const rc = lib.wwatp_quic_stream_read(streamPtr, buf, buf.length, timeoutMs >>> 0);
    const n = Number(rc);
    if (n <= 0) return new Uint8Array();
    return new Uint8Array(buf.subarray(0, n));
  }

  function closeStream(streamPtr) {
    if (streamPtr && !streamPtr.isNull()) lib.wwatp_quic_stream_close(streamPtr);
  }

  return {
    libPath,
    types: { SessionOpts },
    createSession,
    closeSession,
    openBidiStream,
    write,
    read,
    closeStream,
    lastError,
  };
}

export default tryLoadNativeQuic;
