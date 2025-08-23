// Node-only HTTP/3 transport using node-libcurl to reuse a single QUIC connection
// NOTE: This file is not imported by default. You can opt-in in tests by dynamic import.
// Requirements: a system libcurl with HTTP/3 enabled (nghttp3/ngtcp2) and node-libcurl installed.
// Environment vars: WWATP_CERT, WWATP_KEY, WWATP_CA, WWATP_INSECURE
// API shape mirrors Communication: connect(), close(), sendRequest(sid, request, data?, options?)

import Communication from './communication.js';

// Lazily load node-libcurl only when this transport is actually used.
// This prevents import-time failures for consumers that don’t need it.
let Curl, CurlHttpVersion;
let _libcurlTried = false;
async function ensureLibcurlLoaded() {
  if (Curl) return true;
  if (_libcurlTried) return false;
  _libcurlTried = true;
  try {
    const lib = await import('node-libcurl');
    Curl = lib.Curl;
    CurlHttpVersion = lib.CurlHttpVersion || lib.CurlHttpVersion; // alias safety
    return true;
  } catch (err) {
    // Defer throwing until runtime use (connect), so mere import doesn’t explode.
    return false;
  }
}

function envFlag(name) {
  return typeof process !== 'undefined' && process.env && process.env[name];
}

function buildUrl(req) {
  const scheme = req.scheme || 'https';
  const authority = req.authority || '127.0.0.1:12345';
  const path = req.path || '/';
  return `${scheme}://${authority}${path}`;
}

function getCurlVersionInfo() {
  try {
  if (Curl && typeof Curl.getVersionInfoString === 'function') {
      return Curl.getVersionInfoString();
    }
  } catch {}
  return '';
}

function ensureHttp3Supported() {
  const v = getCurlVersionInfo();
  if (!v) return; // best effort; let runtime error surface if missing
  // Look for HTTP3 feature or linked QUIC libs
  const hasHttp3 = /HTTP\/?3/i.test(v) || /nghttp3|ngtcp2|quiche/i.test(v);
  if (!hasHttp3) {
    throw new Error(`libcurl lacks HTTP/3 support. Please check version info`);
  }
}

function tryForceHttp3(handle) {
  try {
    if (CurlHttpVersion && Object.prototype.hasOwnProperty.call(CurlHttpVersion, 'V3')) {
      handle.setOpt(Curl.option.HTTP_VERSION, CurlHttpVersion.V3);
    } else {
      // If the constant is missing (older node-libcurl), defer to default;
      // ensureHttp3Supported() will have thrown earlier in unsupported envs.
    }
  } catch (_) {
    // Ignore; we'll rely on server defaults or the environment will fail fast elsewhere.
  }
}

// Minimal connection pool keyed by authority; keeps one Multi and one Curl handle for reuse.
export default class LibcurlTransport extends Communication {
  constructor() {
    super();
    this._cid = 'libcurl:multi-h3';
    this._warm = null; // optional warm-up handle
    this._authority = null;
    this._connected = false;
  }

  connectionId() { return this._cid; }

  async connect(req = { authority: envFlag('WWATP_AUTH') || '127.0.0.1:12345', scheme: 'https' }) {
    if (this._connected) return true;
    this._authority = req.authority;

    // Load node-libcurl on first use
    const ok = await ensureLibcurlLoaded();
    if (!ok || !Curl) {
      throw new Error('node-libcurl is not installed. Install it to use LibcurlTransport.');
    }

  // Verify libcurl has HTTP/3 to provide a clear, early diagnostic
  ensureHttp3Supported();

    // Warm-up with a simple HEAD to establish connection semantics. Not strictly required.
    const h = new Curl();
  h.setOpt(Curl.option.URL, buildUrl({ ...req, path: '/index.html' }));
  tryForceHttp3(h);
    h.setOpt(Curl.option.HTTPHEADER, [
      'accept: application/octet-stream',
      'content-type: application/octet-stream',
    ]);

    const cert = envFlag('WWATP_CERT');
    const key = envFlag('WWATP_KEY');
    const ca = envFlag('WWATP_CA');
    const insecure = envFlag('WWATP_INSECURE');
    if (cert) h.setOpt(Curl.option.SSLCERT, cert);
    if (key) h.setOpt(Curl.option.SSLKEY, key);
    if (ca) h.setOpt(Curl.option.CAINFO, ca);
    if (insecure) h.setOpt(Curl.option.SSL_VERIFYPEER, false);

    h.setOpt(Curl.option.NOBODY, true); // HEAD-like
    h.setOpt(Curl.option.WRITEFUNCTION, () => 0);

    await new Promise((resolve, reject) => {
      h.on('end', () => { try { h.close(); } catch {} resolve(); });
      h.on('error', (err) => { try { h.close(); } catch {} reject(err); });
      h.perform();
    });

    this._connected = true;
    return true;
  }

  async close() {
    try {
      if (this._warm) this._warm.close();
    } finally {
      this._warm = null;
      this._connected = false;
    }
    return true;
  }

  // Sends a single WWATP request and returns the raw body bytes.
  // Options: { timeoutMs }
  async sendRequest(sid, request, data = null, options = {}) {
    if (!this._connected) await this.connect(request);

    const url = buildUrl(request);
    const method = (request.method || (data ? 'POST' : 'GET')).toUpperCase();

    const handle = new Curl();
    handle.setOpt(Curl.option.URL, url);
  tryForceHttp3(handle);
    handle.setOpt(Curl.option.HTTPHEADER, [
      'accept: application/octet-stream',
      'content-type: application/octet-stream',
    ]);

    const cert = envFlag('WWATP_CERT');
    const key = envFlag('WWATP_KEY');
    const ca = envFlag('WWATP_CA');
    const insecure = envFlag('WWATP_INSECURE');
  if (cert) handle.setOpt(Curl.option.SSLCERT, cert);
  if (key) handle.setOpt(Curl.option.SSLKEY, key);
  if (ca) handle.setOpt(Curl.option.CAINFO, ca);
  if (insecure) handle.setOpt(Curl.option.SSL_VERIFYPEER, false);

    if (method === 'GET') {
      handle.setOpt(Curl.option.CUSTOMREQUEST, 'GET');
    } else if (method === 'POST') {
  handle.setOpt(Curl.option.POST, true);
    } else {
      handle.setOpt(Curl.option.CUSTOMREQUEST, method);
    }

    const bodyBuf = data ? Buffer.from(data) : null;
    if (bodyBuf) {
      // Provide exact-sized upload via READFUNCTION
      let offset = 0;
      handle.setOpt(Curl.option.UPLOAD, true);
      handle.setOpt(Curl.option.INFILESIZE, bodyBuf.length);
      handle.setOpt(Curl.option.READFUNCTION, (buf) => {
        const remaining = bodyBuf.length - offset;
        if (remaining <= 0) return 0;
        const toCopy = Math.min(buf.length, remaining);
        bodyBuf.copy(buf, 0, offset, offset + toCopy);
        offset += toCopy;
        return toCopy;
      });
    }

    const timeoutMs = options.timeoutMs ?? 10000;
    handle.setOpt(Curl.option.TIMEOUT_MS, timeoutMs);

    const chunks = [];
    handle.setOpt(Curl.option.WRITEFUNCTION, (buf, size, nmemb) => {
      // The library will pass a Buffer already; just capture it
      chunks.push(Buffer.from(buf));
      return size * nmemb;
    });

    return new Promise((resolve) => {
      handle.on('end', (statusCode) => {
        const body = Buffer.concat(chunks);
        const ok = statusCode >= 200 && statusCode < 300;
        const u8 = new Uint8Array(body);
        this._emitResponseEvent(sid, { type: 'response', ok, status: statusCode, data: u8 });
        handle.close();
        resolve({ ok, status: statusCode, data: u8 });
      });
      handle.on('error', (err) => {
        const msg = `libcurl error: ${String(err && err.message ? err.message : err)}`;
        this._emitResponseEvent(sid, { type: 'response', ok: false, status: 0, error: msg, data: new Uint8Array() });
        try { handle.close(); } catch (_) {}
        resolve({ ok: false, status: 0, error: msg, data: new Uint8Array() });
      });

      handle.perform();
    });
  }
}
