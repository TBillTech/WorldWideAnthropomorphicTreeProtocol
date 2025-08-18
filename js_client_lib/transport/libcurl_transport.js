// Node-only HTTP/3 transport using node-libcurl to reuse a single QUIC connection
// NOTE: This file is not imported by default. You can opt-in in tests by dynamic import.
// Requirements: a system libcurl with HTTP/3 enabled (nghttp3/ngtcp2) and node-libcurl installed.
// Environment vars: WWATP_CERT, WWATP_KEY, WWATP_CA, WWATP_INSECURE
// API shape mirrors Communication: connect(), close(), sendRequest(sid, request, data?, options?)

import Communication from './communication.js';

let Curl, CurlMulti, CurlFeature, CurlHttpVersion;
try {
  const lib = await import('node-libcurl');
  Curl = lib.Curl;
  CurlMulti = lib.CurlMulti;
  CurlFeature = lib.CurlFeature;
  CurlHttpVersion = lib.CurlHttpVersion || lib.CurlHttpVersion; // alias safety
} catch {
  throw new Error('node-libcurl is not installed. Install it in this workspace to use libcurl_transport.');
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

// Minimal connection pool keyed by authority; keeps one Multi and one Curl handle for reuse.
export default class LibcurlTransport extends Communication {
  constructor() {
    super();
    this._cid = 'libcurl:multi-h3';
    this._multi = null;
    this._easy = null; // main connection seed to establish QUIC session
    this._authority = null;
    this._connected = false;
  }

  connectionId() { return this._cid; }

  async connect(req = { authority: envFlag('WWATP_AUTH') || '127.0.0.1:12345', scheme: 'https' }) {
    if (this._connected) return true;
    this._authority = req.authority;

    this._multi = new CurlMulti();

    // Seed an easy handle to create the H3 connection. We keep it around for reuse.
    this._easy = new Curl();
    this._easy.enable(CurlFeature.Raw);
    this._easy.setOpt('URL', buildUrl({ ...req, path: '/' }));
    this._easy.setOpt('HTTP_VERSION', CurlHttpVersion.V3); // force HTTP/3
    this._easy.setOpt('HTTPHEADER', [
      'accept: application/octet-stream',
      'content-type: application/octet-stream',
    ]);

    const cert = envFlag('WWATP_CERT');
    const key = envFlag('WWATP_KEY');
    const ca = envFlag('WWATP_CA');
    const insecure = envFlag('WWATP_INSECURE');
    if (cert) this._easy.setOpt('SSLCERT', cert);
    if (key) this._easy.setOpt('SSLKEY', key);
    if (ca) this._easy.setOpt('CAINFO', ca);
    if (insecure) this._easy.setOpt('SSL_VERIFYPEER', false);

    // Use a HEAD request to warm up the connection
    this._easy.setOpt('CUSTOMREQUEST', 'HEAD');

    // Quietly ignore data
    this._easy.setOpt('WRITEFUNCTION', () => 0);

    await new Promise((resolve, reject) => {
      this._easy.on('end', () => resolve());
      this._easy.on('error', (err) => reject(err));
      this._multi.addHandle(this._easy);
    });

    this._connected = true;
    return true;
  }

  async close() {
    try {
      if (this._easy) this._easy.close();
      if (this._multi) this._multi.close();
    } finally {
      this._easy = null;
      this._multi = null;
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
    handle.enable(CurlFeature.Raw);
    handle.setOpt('URL', url);
    handle.setOpt('HTTP_VERSION', CurlHttpVersion.V3);
    handle.setOpt('HTTPHEADER', [
      'accept: application/octet-stream',
      'content-type: application/octet-stream',
    ]);

    const cert = envFlag('WWATP_CERT');
    const key = envFlag('WWATP_KEY');
    const ca = envFlag('WWATP_CA');
    const insecure = envFlag('WWATP_INSECURE');
    if (cert) handle.setOpt('SSLCERT', cert);
    if (key) handle.setOpt('SSLKEY', key);
    if (ca) handle.setOpt('CAINFO', ca);
    if (insecure) handle.setOpt('SSL_VERIFYPEER', false);

    if (method === 'GET') {
      handle.setOpt('CUSTOMREQUEST', 'GET');
    } else if (method === 'POST') {
      handle.setOpt('POST', true);
    } else {
      handle.setOpt('CUSTOMREQUEST', method);
    }

    const bodyBuf = data ? Buffer.from(data) : null;
    if (bodyBuf) {
      // Provide exact-sized upload via READFUNCTION
      let offset = 0;
      handle.setOpt('UPLOAD', true);
      handle.setOpt('INFILESIZE', bodyBuf.length);
      handle.setOpt('READFUNCTION', (buf) => {
        const remaining = bodyBuf.length - offset;
        if (remaining <= 0) return 0;
        const toCopy = Math.min(buf.length, remaining);
        bodyBuf.copy(buf, 0, offset, offset + toCopy);
        offset += toCopy;
        return toCopy;
      });
    }

    const timeoutMs = options.timeoutMs ?? 10000;
    handle.setOpt('TIMEOUT_MS', timeoutMs);

    const chunks = [];
    handle.setOpt('WRITEFUNCTION', (buf, size, nmemb) => {
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

      this._multi.addHandle(handle);
    });
  }
}
