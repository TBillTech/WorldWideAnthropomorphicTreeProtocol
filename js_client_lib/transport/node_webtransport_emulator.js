/* eslint-env node */
// Node WebTransport emulator for Node.js that wraps the native QUIC addon (N-API).
// It mimics the browser WebTransport surface enough for WebTransportCommunication
// style adapters or direct use in Node tests. Single QUIC session per instance.

import { tryLoadNativeQuic } from './native_quic.js';
import { tracer } from './instrumentation.js';

function parseUrl(u) {
  try {
    // eslint-disable-next-line no-undef
    return new URL(u);
  } catch {
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
  this._openStreams = new Set();
  this._sidToStream = new Map(); // map of String(sid) -> native stream handle
  this._streamToSid = new Map(); // reverse map for cleanup
  this._lastOpened = null;
  this._trace = tracer('NodeWebTransportEmulator');
  this._pumpId = null; // background pump interval id
  // Numeric client connection id if available from native addon (best-effort)
  this._clientCid = undefined;
  // Simple readiness hints for readable sides; keyed by native stream handle (object identity not exposed)
  this._readReadyFlag = false;

    this.closed = new Promise((res, rej) => { this._closedResolve = res; this._closedReject = rej; });
    this.draining = new Promise((res) => { this._drainingResolve = res; });
    this.datagrams = {
      readable: (typeof ReadableStream !== 'undefined')
        // eslint-disable-next-line no-undef
        ? new ReadableStream({ start(controller) { controller.close(); } })
        : { getReader: () => ({ read: async () => ({ done: true }) }) },
      createWritable() {
        if (typeof WritableStream !== 'undefined') {
          // eslint-disable-next-line no-undef
          return new WritableStream({ write() { throw new Error('QUIC DATAGRAM not supported'); } });
        }
        return { getWriter: () => ({ write() { throw new Error('QUIC DATAGRAM not supported'); } }) };
      },
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
  this._trace.info('session.create', { url: opts.url });
  this._session = this._nq.createSession(opts);
  // Try to retrieve a stable numeric client connection id from addon if exposed
  try {
    const cid = typeof this._nq.getClientConnectionId === 'function'
      ? this._nq.getClientConnectionId(this._session)
      : undefined;
    if (cid !== undefined && cid !== null) this._clientCid = Number(cid);
  } catch (_) {}
  this._trace.info('session.ready');
  // Start a lightweight background pump to drive native request/response processing,
  // mirroring libcurl's multi interface behavior. This ensures progress even when
  // callers don't invoke processRequestStream frequently.
  try {
    if (!this._pumpId && this._nq && typeof this._nq.processRequestStream === 'function') {
      const period = Math.max(5, Number(this.options?.pumpIntervalMs ?? 20));
      this._pumpId = setInterval(() => {
        try {
          const rc = this._nq.processRequestStream(this._session);
          if (rc > 0) {
            this._trace.inc('proc.progress', rc);
            // Mark that at least one stream likely has data available
            this._readReadyFlag = true;
          }
        } catch {}
      }, period);
    }
  } catch {}
      // If creation failed, createSession would have thrown.
    })();
    // Attach a catch to avoid unhandled rejection warnings, but keep ready as the original promise
    initPromise.catch((e) => { try { this._closedReject?.(e); } catch {} });
    this.ready = initPromise;
  }

  get reliability() { return this._reliability; }
  get congestionControl() { return this._congestionControl; }
  get protocol() { return this._protocol; }
  // Optional helper for consumers to read numeric client CID
  async getClientConnectionId() { return this._clientCid; }

  async close(closeInfo = {}) {
    // Graceful close: free session
    try {
      // First, best-effort close any open streams once
      for (const st of Array.from(this._openStreams)) {
        try { this._nq?.closeStream(st); } catch {}
        this._openStreams.delete(st);
      }
  if (this._pumpId) { try { clearInterval(this._pumpId); } catch {} this._pumpId = null; }
  if (this._session && this._nq) {
        this._trace.info('session.close');
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
  this._openStreams.add(st);
  this._lastOpened = st;
  // Optional: bind this native stream to a caller-provided sid for later targeted close
  try {
    const sidOpt = _options && _options.sid !== undefined ? _options.sid : null;
    if (sidOpt !== null) {
      const key = String(sidOpt);
      this._sidToStream.set(key, st);
      this._streamToSid.set(st, key);
    }
  } catch {}
  this._trace.inc('streams.open');
  // Avoid coercing native handle to primitive; just tag type safely
  const safeTag = (x) => { try { return Object.prototype.toString.call(x); } catch { return undefined; } };
  this._trace.info('stream.open', { idTag: safeTag(st), count: this._openStreams.size });

    const self = this;
    // Writer
    let writerClosed = false;
  let streamClosed = false; // guard to ensure native close called once
  const writer = {
      async write(chunk) {
        if (writerClosed) throw new Error('writer closed');
        const u8 = chunk instanceof Uint8Array ? chunk : new Uint8Array(chunk);
        const wrote = self._nq.write(st, u8, false);
        if (wrote < 0) throw new Error(`write failed: ${self._nq.lastError?.() || 'unknown'}`);
        self._bytesSent += wrote;
        self._trace.inc('bytes.sent', wrote);
      },
      async close() {
        if (writerClosed) return;
        writerClosed = true;
        const wrote = self._nq.write(st, new Uint8Array(0), true);
        if (wrote < 0) throw new Error(`close(write FIN) failed: ${self._nq.lastError?.() || 'unknown'}`);
        self._trace.info('stream.fin', { idTag: safeTag(st) });
      },
      async abort(_reason) {
        writerClosed = true;
        if (!streamClosed) {
          streamClosed = true;
          try { self._nq.closeStream(st); } catch {}
          self._openStreams.delete(st);
          try {
            const key = self._streamToSid.get(st);
            if (key) { self._sidToStream.delete(key); self._streamToSid.delete(st); }
          } catch {}
          self._trace.inc('streams.closed');
          self._trace.info('stream.abort', { idTag: safeTag(st), count: self._openStreams.size });
        }
      },
      releaseLock() {},
      get closed() { return Promise.resolve(); },
      get ready() { return Promise.resolve(); },
    };

    // Readable
    let done = false;
    let reading = false; // guard against overlapping read invocations
    const reader = {
      async read() {
        if (done) return { value: undefined, done: true };
        if (reading) { await new Promise((r) => setTimeout(r, 5)); return { value: undefined, done: false }; }
        reading = true;
        // Drain all currently-ready chunks from native into a single buffer
        const bufs = [];
        let total = 0;
        const tryConcat = () => {
          if (bufs.length === 1) return bufs[0];
          const out = new Uint8Array(total);
          let offset = 0;
          for (const b of bufs) { out.set(b, offset); offset += b.byteLength; }
          return out;
        };
        // First attempt: short blocking read to kick things
        let out = self._nq.read(st, 65536, 50);
        if (out instanceof Uint8Array && out.byteLength > 0) {
          bufs.push(out);
          total += out.byteLength;
          // Keep draining as long as native signals readiness
          try {
            while (self._nq && typeof self._nq.readReady === 'function' && self._session && self._nq.readReady(self._session, Number(self._lastOpened ?  self._lastOpened.logicalId || 0 : 0))) {
              const more = self._nq.read(st, 65536, 0);
              if (!(more instanceof Uint8Array) || more.byteLength === 0) break;
              bufs.push(more);
              total += more.byteLength;
            }
          } catch {}
          const merged = tryConcat();
          self._bytesReceived += merged.byteLength;
          self._trace.inc('bytes.recv', merged.byteLength);
          self._readReadyFlag = false;
          reading = false;
          return { value: merged, done: false };
        }
        if (out instanceof Uint8Array && out.byteLength === 0) {
          // No data yet; yield and try again soon
          await new Promise((r) => setTimeout(r, 10));
          reading = false;
          return { value: undefined, done: false };
        }
        if (out && typeof out === 'object' && out.fin === true) {
          done = true;
          reading = false;
          return { value: undefined, done: true };
        }
        await new Promise((r) => setTimeout(r, 10));
        reading = false;
        return { value: undefined, done: false };
      },
      async cancel(_reason) {
        done = true;
        if (!streamClosed) {
          streamClosed = true;
          try { self._nq.closeStream(st); } catch {}
          self._openStreams.delete(st);
          try {
            const key = self._streamToSid.get(st);
            if (key) { self._sidToStream.delete(key); self._streamToSid.delete(st); }
          } catch {}
          self._trace.inc('streams.closed');
          self._trace.info('stream.cancel', { idTag: safeTag(st), count: self._openStreams.size });
        }
      },
      releaseLock() {},
    };

    return {
      readable: { getReader: () => reader },
      writable: { getWriter: () => writer },
    };
  }

  // Optional: Tag the WWATP request signal for the most recently opened stream
  setRequestSignalForCurrentStream(sig) {
    if (!this._lastOpened || !this._nq?.setRequestSignal) return;
    try { this._nq.setRequestSignal(this._lastOpened, sig >>> 0); } catch {}
  }

  // Pump the underlying native connector request/response queues once.
  // Returns true if any progress was made.
  processRequestStream() {
    if (!this._nq || !this._session || typeof this._nq.processRequestStream !== 'function') return false;
    try {
      const rc = this._nq.processRequestStream(this._session);
      if (rc > 0) this._readReadyFlag = true;
      return rc > 0;
    } catch {
      return false;
    }
  }

  // Readiness hint for readable data for a specific logical stream id (sid).
  // Prefer the native addon's per-stream readReady(session,sid) when available;
  // fall back to a coarse sticky flag if the addon doesn't provide it.
  readReady(sid) {
    try {
      if (this._nq && typeof this._nq.readReady === 'function' && this._session) {
        return !!this._nq.readReady(this._session, Number(sid));
      }
    } catch {}
    return this._readReadyFlag === true;
  }

  async createUnidirectionalStream() {
    // Not implemented: servers currently expect bidi for requests
    throw new Error('createUnidirectionalStream not implemented');
  }

  createSendGroup() {
    return { add: async () => {}, wait: async () => {} };
  }

  // Allow explicit close of the most recent stream for a given sid in our simple model.
  // In this emulator, we don't track sid->native stream mapping, so close the last opened.
  async closeStream(_sid) {
    const key = String(_sid);
    const st = this._sidToStream.get(key) || null;
    if (!st) return;
    try { this._nq?.closeStream(st); } catch {}
    this._openStreams.delete(st);
    try { this._sidToStream.delete(key); this._streamToSid.delete(st); } catch {}
    this._trace.inc('streams.closed');
    this._trace.info('stream.closed', { idTag: Object.prototype.toString.call(st), count: this._openStreams.size });
  }
}
