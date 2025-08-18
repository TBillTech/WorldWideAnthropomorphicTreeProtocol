// Node WebTransport emulator for Node.js that wraps the native QUIC addon (N-API).
// It mimics the browser WebTransport surface enough for WebTransportCommunication
// style adapters or direct use in Node tests. Single QUIC session per instance.

import { tryLoadNativeQuic } from './native_quic.js';

function parseUrl(u) {
  try {
    return new URL(u);
  } catch (e) {
    throw new TypeError(`Invalid URL for WebTransport: ${u}`);
  }
}

function envFlag(name) {
  try { return !!(process?.env?.[name]); } catch { return false; }
}

function envStr(name) {
  try { return process?.env?.[name] || null; } catch { return null; }
}

export default class NodeWebTransportEmulator {
  static supportsReliableOnly = false;

  constructor(url, options = {}) {
    this.url = String(url);
    this.options = options || {};
    this._nq = null;
    this._session = null;
    this._closedResolve = null;
    this._closedReject = null;
    this._drainingResolve = null;
    this._bytesSent = 0;
    this._bytesReceived = 0;
    this._protocol = 'h3';
    this._reliability = 'reliable-only';
    this._congestionControl = options?.congestionControl || 'default';

    this.closed = new Promise((res, rej) => { this._closedResolve = res; this._closedReject = rej; });
    this.draining = new Promise((res) => { this._drainingResolve = res; });
    this.datagrams = {
      readable: new ReadableStream({ start(controller) { controller.close(); } }),
      createWritable() { return new WritableStream({ write() { throw new Error('QUIC DATAGRAM not supported'); } }); },
      maxDatagramSize: 0,
      incomingMaxAge: 0,
      outgoingMaxAge: 0,
      incomingHighWaterMark: 0,
      outgoingHighWaterMark: 0,
    };

    const u = parseUrl(this.url);
    if (u.protocol !== 'https:') {
      const err = new TypeError('WebTransport requires https URL in Node emulator');
      this.ready = Promise.reject(err);
      this._closedReject?.(err);
      return;
    }

    // Lazy-create the QUIC session
    const initPromise = (async () => {
      const nq = tryLoadNativeQuic();
      if (!nq) throw new Error('Native QUIC addon not available');
      this._nq = nq;
      const opts = {
        url: `${u.protocol}//${u.host}`,
      };
      const cert = this.options?.cert || envStr('WWATP_CERT');
      const key = this.options?.key || envStr('WWATP_KEY');
      const ca = this.options?.ca || envStr('WWATP_CA');
      const insecure = !!(this.options?.insecure || envFlag('WWATP_INSECURE'));
      // Enforce mTLS by default: require cert+key unless explicitly marked insecure.
      if (!insecure && !(cert && key)) {
        throw new Error('WebTransport (Node) requires WWATP_CERT and WWATP_KEY (client mTLS) or set WWATP_INSECURE=1 for dev.');
      }
      if (cert && key) { opts.cert_file = cert; opts.key_file = key; }
      if (ca) opts.ca_file = ca; else if (insecure) opts.insecure_skip_verify = true;
      // Create a single connection (session)
      this._session = this._nq.createSession(opts);
      // If creation failed, createSession would have thrown.
    })();
    // Attach a catch to avoid unhandled rejection warnings, but keep ready as the original promise
    initPromise.catch((e) => { try { this._closedReject?.(e); } catch {} });
    this.ready = initPromise;
  }

  get reliability() { return this._reliability; }
  get congestionControl() { return this._congestionControl; }
  get protocol() { return this._protocol; }

  async close(closeInfo = {}) {
    // Graceful close: free session
    try {
      if (this._session && this._nq) {
        this._nq.closeSession(this._session);
      }
      this._session = null;
      this._drainingResolve?.();
      this._closedResolve?.({ closeCode: closeInfo?.closeCode || 0, reason: closeInfo?.reason || '' });
    } catch (e) {
      this._closedReject?.(e);
      throw e;
    }
  }

  async getStats() {
    return {
      bytesSent: this._bytesSent,
      bytesReceived: this._bytesReceived,
      packetsSent: 0,
      packetsReceived: 0,
      timestamp: Date.now(),
    };
  }

  // Minimal support for keying material
  async exportKeyingMaterial() {
    throw new Error('exportKeyingMaterial not supported by Node emulator');
  }

  async createBidirectionalStream(_options = {}) {
    await this.ready;
    if (!this._session) throw new Error('Session not open');
    const path = parseUrl(this.url).pathname || '/';
    const st = this._nq.openBidiStream(this._session, path);
    if (!st) throw new Error('Failed to open bidirectional stream');

    const self = this;
    // Writer
    let writerClosed = false;
    const writer = {
      async write(chunk) {
        if (writerClosed) throw new Error('writer closed');
        const u8 = chunk instanceof Uint8Array ? chunk : new Uint8Array(chunk);
        const wrote = self._nq.write(st, u8, false);
        if (wrote < 0) throw new Error(`write failed: ${self._nq.lastError?.() || 'unknown'}`);
        self._bytesSent += wrote;
      },
      async close() {
        if (writerClosed) return;
        writerClosed = true;
        const wrote = self._nq.write(st, new Uint8Array(0), true);
        if (wrote < 0) throw new Error(`close(write FIN) failed: ${self._nq.lastError?.() || 'unknown'}`);
      },
      async abort(_reason) {
        writerClosed = true;
        try { self._nq.closeStream(st); } catch {}
      },
      releaseLock() {},
      get closed() { return Promise.resolve(); },
      get ready() { return Promise.resolve(); },
    };

    // Readable
    let done = false;
    const reader = {
      async read() {
        if (done) return { value: undefined, done: true };
        const out = self._nq.read(st, 65536, 10);
        if (!(out instanceof Uint8Array) || out.byteLength === 0) {
          // Interpret zero-length read as end-of-stream
          done = true;
          return { value: undefined, done: true };
        }
        self._bytesReceived += out.byteLength;
        return { value: out, done: false };
      },
      async cancel(_reason) {
        done = true;
        try { self._nq.closeStream(st); } catch {}
      },
      releaseLock() {},
    };

    return {
      readable: { getReader: () => reader },
      writable: { getWriter: () => writer },
    };
  }

  async createUnidirectionalStream() {
    // Not implemented: servers currently expect bidi for requests
    throw new Error('createUnidirectionalStream not implemented');
  }

  createSendGroup() {
    return { add: async () => {}, wait: async () => {} };
  }
}
