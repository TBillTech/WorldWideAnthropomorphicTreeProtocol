// WebTransport-based transport adapter (browser-only). Requires secure context and server support.
// Exposes a simple sendRequest API over a new bidirectional stream per request.
// Note: WebTransport API availability varies; callers should feature-detect.

import Communication from './communication.js';
import { tracer } from './instrumentation.js';

// Small helper to coerce various input types into async chunks for writing
async function* toAsyncChunks(data) {
    if (!data) return;
    if (data instanceof Uint8Array) {
        yield data;
        return;
    }
    if (data.buffer instanceof ArrayBuffer && typeof data.byteLength === 'number') {
        yield new Uint8Array(data.buffer, data.byteOffset || 0, data.byteLength);
        return;
    }
    if (typeof data === 'string') {
        yield new TextEncoder().encode(data);
        return;
    }
    // ReadableStream<Uint8Array>
    if (typeof data.getReader === 'function') {
        const reader = data.getReader();
        try {
            while (true) {
                const { value, done } = await reader.read();
                if (done) break;
                if (value) yield value instanceof Uint8Array ? value : new Uint8Array(value);
            }
        } finally {
            try { reader.releaseLock?.(); } catch {}
        }
        return;
    }
    // AsyncIterable
    if (Symbol.asyncIterator in Object(data)) {
        for await (const chunk of data) {
            if (chunk) yield chunk instanceof Uint8Array ? chunk : new Uint8Array(chunk);
        }
        return;
    }
}

export default class WebTransportCommunication extends Communication {
    constructor(url) {
        super();
        this.url = url;
        this.transport = null;
        this._cid = `webtransport:${url}`;
    this._trace = tracer('WebTransportCommunication');
    }

    connectionId() {
        return this._cid;
    }

    async connect() {
        if (typeof WebTransport === 'undefined') {
            throw new Error('WebTransport is not available in this environment');
        }
        this._trace.info('connect.start', { url: this.url });
        this.transport = new WebTransport(this.url);
        await this.transport.ready;
        this._connected = true;
        // closed is a Promise on the WebTransport instance
        this.transport.closed.then((ci) => {
            this._connected = false;
            this._trace.info('closed.resolve', { closeInfo: ci });
        }).catch((e) => {
            this._connected = false;
            this._trace.warn('closed.reject', { error: String(e && e.message || e) });
        });
        this._trace.info('connect.ready');
        return true;
    }

    async close() {
        if (this.transport) {
            this._trace.info('close.start');
            await this.transport.close();
            this._trace.info('close.done');
        }
        this._connected = false;
        return true;
    }

    async sendRequest(sid, _request, data = null, options = {}) {
        if (!this.transport) throw new Error('Not connected');
        const { timeoutMs = 0, signal } = options || {};

    const bidi = await this.transport.createBidirectionalStream();
    this._trace.inc('streams.open');
    this._trace.info('stream.open', { sid });
        const writer = bidi.writable.getWriter();
        const reader = bidi.readable.getReader();

        let aborted = false;
        let timeoutId = null;
        let abortReject = null;
        const onAbort = () => {
            aborted = true;
            try { writer.abort?.(new DOMException('Aborted', 'AbortError')); } catch {}
            try { reader.cancel?.('aborted'); } catch {}
            try { abortReject?.(new DOMException('Aborted', 'AbortError')); } catch {}
        };
        if (signal) {
            if (signal.aborted) onAbort();
            else signal.addEventListener('abort', onAbort, { once: true });
        }
        if (timeoutMs && timeoutMs > 0) {
            timeoutId = setTimeout(() => onAbort(), timeoutMs);
        }

        try {
            // Write request body as-is
            if (data) {
                for await (const chunk of toAsyncChunks(data)) {
                    if (aborted) throw new DOMException('Aborted', 'AbortError');
                    await writer.write(chunk);
                }
            }
            // Make close abortable by racing with abort/timeout
            const abortPromise = new Promise((_, rej) => { abortReject = rej; });
            const res = await Promise.race([
                writer.close(),
                abortPromise,
            ]);
            if (aborted) {
                throw new DOMException('Aborted', 'AbortError');
            }

            // Read full response into a single buffer
            const chunks = [];
            while (true) {
                const { value, done } = await reader.read();
                if (done) break;
                if (value) chunks.push(value instanceof Uint8Array ? value : new Uint8Array(value));
            }
            const totalLen = chunks.reduce((a, c) => a + c.byteLength, 0);
            const out = new Uint8Array(totalLen);
            let off = 0;
            for (const c of chunks) { out.set(c, off); off += c.byteLength; }
            this._emitResponseEvent(sid, { type: 'response', ok: true, status: 200, data: out });
            this._trace.info('stream.response', { sid, bytes: out.byteLength });
            return { ok: true, status: 200, data: out };
        } catch (err) {
            // Surface error; consumer may also rely on thrown error
            this._emitResponseEvent(sid, { type: 'error', ok: false, status: 0, error: err });
            this._trace.warn('stream.error', { sid, error: String(err && err.message || err) });
            throw err;
        } finally {
            try { reader.releaseLock?.(); } catch {}
            try { writer.releaseLock?.(); } catch {}
            this._trace.inc('streams.closed');
            this._trace.info('stream.closed', { sid });
            if (signal) {
                try { signal.removeEventListener('abort', onAbort); } catch {}
            }
            if (timeoutId) clearTimeout(timeoutId);
        }
    }
}
