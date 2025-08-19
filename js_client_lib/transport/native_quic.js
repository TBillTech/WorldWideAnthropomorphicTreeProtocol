// Minimal Node N-API loader for libwwatp_quic (C facade).
// Prefer a compiled addon to avoid runtime dlopen FFI issues.

import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import { createRequire } from 'module';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const repoRoot = path.resolve(__dirname, '..', '..');

// No FFI fallback — addon only.

function tryLoadNativeAddon() {
  // Try to load native N-API addon built under js_client_lib/native/build/Release
  try {
    const requireCjs = createRequire(import.meta.url);
    const addonPath = path.resolve(repoRoot, 'js_client_lib', 'native', 'build', 'Release', 'wwatp_quic_native.node');
    if (fs.existsSync(addonPath)) {
      const addon = requireCjs(addonPath);
      return addon;
    }
  } catch (_) {}
  return null;
}

export function tryLoadNativeQuic() {
  // Only in Node
  if (typeof process === 'undefined' || !process.versions?.node) return null;
  // Prefer N-API addon if present
  const addon = tryLoadNativeAddon();
  if (addon) {
    // Wrap addon methods with same API shape as FFI path
  function createSession(opts) { return addon.createSession(opts); }
    function closeSession(sess) { try { addon.closeSession(sess); } catch {} }
    function openBidiStream(sess, p) { return addon.openBidiStream(sess, p); }
  function setRequestSignal(st, sig) { try { addon.setRequestSignal(st, sig >>> 0); } catch {} }
  function processRequestStream(sess) { try { return Number(addon.processRequestStream(sess)) || 0; } catch { return 0; } }
    function write(st, data, endStream = true) {
      const u8 = Buffer.isBuffer(data) ? new Uint8Array(data) : (data instanceof Uint8Array ? data : new Uint8Array());
      return Number(addon.write(st, u8, !!endStream));
    }
    function read(st, maxLen = 65536, timeoutMs = 0) { return addon.read(st, maxLen >>> 0, timeoutMs >>> 0); }
    function closeStream(st) { try { addon.closeStream(st); } catch {} }
  function lastError() { try { return addon.lastError(); } catch { return 'unknown'; } }
  // Optional: expose numeric client CID if addon supports it
  function getClientConnectionId(sess) { try { return addon.getClientConnectionId?.(sess); } catch { return undefined; } }
  return { libPath: '(napi addon)', types: {}, createSession, closeSession, openBidiStream, setRequestSignal, write, read, closeStream, lastError, getClientConnectionId, processRequestStream };
  }
  // If addon not found, return null (no fallback).
  return null;
}

export default tryLoadNativeQuic;
export function NativeQuic(libPath) { return tryLoadNativeQuic(libPath); }
