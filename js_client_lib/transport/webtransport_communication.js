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
    // Default CID is a readable label; will be replaced with a numeric QUIC
    // client connection id if the underlying transport exposes one (Node emulator).
    this._cid = `webtransport:${url}`;
    this._trace = tracer('WebTransportCommunication');
    // Maintain a persistent bidi stream per StreamIdentifier to match libcurl orchestration
    // Map key: String(sid) -> { bidi, writer, reader }
    this._streams = new Map();
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
        // Prefer a numeric QUIC client connection id when available (Node emulator).
        try {
            const getCid = this.transport && typeof this.transport.getClientConnectionId === 'function'
                ? this.transport.getClientConnectionId.bind(this.transport)
                : (this.transport && this.transport._clientCid !== undefined ? () => this.transport._clientCid : null);
            if (getCid) {
                const cidVal = await getCid();
                if (cidVal !== undefined && cidVal !== null) {
                    // Store as numeric-like string to align with other transports
                    this._cid = String(cidVal);
                }
            }
        } catch (_) {
            // Best-effort only; keep default label if not available
        }
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

    // Pump request/response queues if the underlying transport supports it (Node emulator path).
    processRequestStream() {
        try {
            let did = false;
            if (this.transport && typeof this.transport.processRequestStream === 'function') {
                did = !!this.transport.processRequestStream();
            }
            // After pumping native queues, proactively nudge reads on all active streams.
            // This helps surface any bytes that were queued by native callbacks immediately.
            try {
                for (const [sidKey, ent] of this._streams.entries()) {
                    // Prefer the original sid object if we stored it; fall back to the key string.
                    const sid = ent && ent.sid ? ent.sid : sidKey;
                    // Fire-and-forget; nudgeRead will gate on transport.readReady when available.
                    this.nudgeRead(sid);
                }
            } catch (_) {}
            return did;
        } catch {}
        return false;
    }

    // Proactively trigger a single read on a persistent Node emulator stream to pull any queued bytes.
    async nudgeRead(sid) {
        try {
            const isNodeEmulator = !!(this.transport && (
                typeof this.transport.processRequestStream === 'function' ||
                typeof this.transport.setRequestSignalForCurrentStream === 'function' ||
                (this.transport && typeof this.transport.getClientConnectionId === 'function')
            ));
            if (!isNodeEmulator) return;
            const sidKey = String(sid);
            const ent = this._streams.get(sidKey);
            if (!ent || !ent.reader) return;
            // Gate on transport-provided readiness hint when available to avoid hot-loop reads
            try {
                if (this.transport && typeof this.transport.readReady === 'function') {
                    // If readReady returns falsy, skip the actual read for now
                    const ready = !!this.transport.readReady(sid);
                    if (!ready) return;
                }
            } catch (_) {}
            // Fire and forget a single read; if data arrives, emit it.
            const { value, done } = await ent.reader.read();
            if (!done && value instanceof Uint8Array && value.byteLength > 0) {
                this._emitResponseEvent(sid, { type: 'response', ok: true, status: 200, data: value });
                this._trace.info('stream.response', { sid, bytes: value.byteLength });
            }
        } catch (_) {
            // best-effort nudge only
        }
    }

    async sendRequest(sid, _request, data = null, options = {}) {
        if (!this.transport) throw new Error('Not connected');
        const { timeoutMs = 0, signal, requestSignal, isHeartbeat = false } = options || {};
        const isNodeEmulator = !!(this.transport && (
            typeof this.transport.processRequestStream === 'function' ||
            typeof this.transport.setRequestSignalForCurrentStream === 'function' ||
            (this.transport && typeof this.transport.getClientConnectionId === 'function')
        ));

        // Stream acquisition strategy:
        // - Node emulator: keep a persistent bidi stream per sid and reuse it for heartbeats and reads
        // - Polyfill/browser: use a fresh bidi stream per request (our polyfill responds once per stream)
        const sidKey = String(sid);
        let entry;
        let created = false;
    if (isNodeEmulator) {
            entry = this._streams.get(sidKey);
            if (!entry) {
                const bidi = await this.transport.createBidirectionalStream();
                const writer = bidi.writable.getWriter();
                const reader = bidi.readable.getReader();
                entry = { bidi, writer, reader, sid };
                this._streams.set(sidKey, entry);
                created = true;
                this._trace.inc('streams.open');
                this._trace.info('stream.open', { sid });
                // If emulator exposes a way to tag WWATP request signal on the native stream, do it before first write
                try {
                    if (requestSignal !== undefined && typeof this.transport?.setRequestSignalForCurrentStream === 'function') {
                        this.transport.setRequestSignalForCurrentStream(requestSignal >>> 0);
                    }
                } catch {}
            }
            // Heartbeats share the same persistent stream so the native layer keeps the same logical id.
            // Do NOT open a new stream for heartbeats on the Node emulator.
        } else {
            // Ephemeral stream for polyfill/browser environments
            const bidi = await this.transport.createBidirectionalStream();
            const writer = bidi.writable.getWriter();
            const reader = bidi.readable.getReader();
            entry = { bidi, writer, reader, sid };
            created = true;
            this._trace.inc('streams.open');
            this._trace.info('stream.open', { sid });
        }
        const { writer, reader } = entry;

        // Start a lightweight background reader pump for persistent Node emulator streams
        if (isNodeEmulator && !isHeartbeat && created) {
            const ent = this._streams.get(sidKey);
            ent._reading = false;
            ent._pumpId = setInterval(async () => {
                // If stream has been closed/removed, stop the pump
                const current = this._streams.get(sidKey);
                if (!current || current !== ent) { try { clearInterval(ent._pumpId); } catch {} return; }
                if (ent._reading) return;
                ent._reading = true;
                try {
                    const { value, done } = await ent.reader.read();
                    if (done) { /* keep interval; reader may signal done transiently */ }
                    else if (value instanceof Uint8Array && value.byteLength > 0) {
                        this._emitResponseEvent(sid, { type: 'response', ok: true, status: 200, data: value });
                        this._trace.info('stream.response', { sid, bytes: value.byteLength });
                    }
                } catch (e) {
                    this._trace.warn('stream.read.error', { sid, error: String(e && e.message || e) });
                } finally {
                    ent._reading = false;
                }
            }, 20);
        }

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
            // Write request body as-is. On Node emulator persistent streams we intentionally keep the
            // writer open (no FIN) so that heartbeats can be written on the same logical stream id.
            if (data) {
                for await (const chunk of toAsyncChunks(data)) {
                    if (aborted) throw new DOMException('Aborted', 'AbortError');
                    await writer.write(chunk);
                }
            }
            // For non-emulator environments (or when using ephemeral streams), FIN after write.
            // For Node emulator persistent streams, DO NOT FIN here; we'll FIN during closeStream(sid).
            if (!isNodeEmulator) {
                try {
                    const closeP = writer.close?.();
                    const waitMs = Math.max(10, Math.min(1000, Number(timeoutMs) || 100));
                    if (closeP && typeof closeP.then === 'function') {
                        await Promise.race([
                            closeP,
                            new Promise((r) => setTimeout(r, waitMs)),
                        ]);
                    }
                } catch {}
            }
            // Keep the stream's reader open; for Node emulator, the persistent reader remains until explicit closeStream(sid).

            // Read response with bounded wait:
            // For Node emulator persistent streams, skip inline read and rely on the background
            // reader pump started above to emit responses. Heartbeats remain ephemeral and use
            // the short bounded read path below.
            if (isNodeEmulator) {
                this._trace.info('stream.defer.read', { sid });
                // Proactively nudge a read once to pick up any bytes enqueued by the native callback.
                try { await this.nudgeRead(sid); } catch {}
                return { ok: true, status: 202, data: new Uint8Array(0) };
            }
            
            // - If we see some data, use a small drain window to accumulate.
            // - If we see no data at all within maxWaitMs, return without emitting (let caller/heartbeats drive progress).
            const chunks = [];
            let lastDataTs = 0;
            const drainMs = 40; // small stabilization window when data has started flowing
            const startTs = Date.now();
            // Default max wait is short for fire-and-pulse semantics; map timeoutMs to read window if provided.
            const maxWaitMs = Math.max(0, Number(options?.readWindowMs ?? (timeoutMs || 100)));
            while (true) {
                if (aborted) throw new DOMException('Aborted', 'AbortError');
                const { value, done } = await reader.read();
                if (done) break;
                if (value instanceof Uint8Array && value.byteLength > 0) {
                    chunks.push(value);
                    lastDataTs = Date.now();
                    continue;
                }
                // No data this tick: if nothing has arrived yet and max wait exceeded, give up for now.
                if (chunks.length === 0 && (Date.now() - startTs) >= maxWaitMs) {
                    break;
                }
                // If data has arrived before, check drain window.
                if (chunks.length > 0 && (Date.now() - lastDataTs) >= drainMs) break;
            }
            const totalLen = chunks.reduce((a, c) => a + c.byteLength, 0);
            if (totalLen > 0) {
                const out = new Uint8Array(totalLen);
                let off = 0;
                for (const c of chunks) { out.set(c, off); off += c.byteLength; }
                this._emitResponseEvent(sid, { type: 'response', ok: true, status: 200, data: out });
                this._trace.info('stream.response', { sid, bytes: out.byteLength });
                return { ok: true, status: 200, data: out };
            } else {
                if (aborted) {
                    const err = new DOMException('Aborted', 'AbortError');
                    this._emitResponseEvent(sid, { type: 'error', ok: false, status: 0, error: err });
                    throw err;
                }
                // If no data arrived and a timeout window was provided in a non-emulator environment,
                // surface a timeout to match polyfill tests' expectations.
                if (!isNodeEmulator && maxWaitMs > 0) {
                    const err = new Error('timeout waiting for response');
                    this._emitResponseEvent(sid, { type: 'error', ok: false, status: 0, error: err });
                    throw err;
                }
                // Avoid emitting zero-byte responses; leave handler registered for future pulses.
                this._trace.info('stream.response.empty', { sid });
                return { ok: true, status: 204, data: new Uint8Array(0) };
            }
        } catch (err) {
            // Surface error; consumer may also rely on thrown error
            this._emitResponseEvent(sid, { type: 'error', ok: false, status: 0, error: err });
            this._trace.warn('stream.error', { sid, error: String(err && err.message || err) });
            throw err;
        } finally {
            // Cleanup locks conditionally based on transport type
            // - Browser/polyfill ephemeral streams: fully cancel
            // - Node emulator persistent streams: keep reader attached; only release writer lock
            if (!isNodeEmulator) {
                try { reader.cancel?.('done'); } catch {}
                try { reader.releaseLock?.(); } catch {}
                try { writer.releaseLock?.(); } catch {}
            } else {
                // Persistent Node emulator stream: keep reader attached for background pump
                try { writer.releaseLock?.(); } catch {}
            }
            if (signal) {
                try { signal.removeEventListener('abort', onAbort); } catch {}
            }
            if (timeoutId) clearTimeout(timeoutId);
        }
    }

    // Explicitly close and cleanup a persistent stream for the given sid.
    async closeStream(sid) {
        const sidKey = String(sid);
        const entry = this._streams.get(sidKey);
        if (!entry) return;
        this._streams.delete(sidKey);
    try { if (entry._pumpId) { clearInterval(entry._pumpId); entry._pumpId = null; } } catch {}
        try {
            // Attempt a graceful FIN then cancel reader to free native stream
            try { await entry.writer.close?.(); } catch {}
            try { await entry.reader.cancel?.('done'); } catch {}
        } finally {
            try { entry.reader.releaseLock?.(); } catch {}
            try { entry.writer.releaseLock?.(); } catch {}
            this._trace.inc('streams.closed');
            this._trace.info('stream.closed', { sid });
            // Also pass-through to underlying transport if it exposes closeStream
            try {
                if (typeof this.transport?.closeStream === 'function') {
                    await this.transport.closeStream(sid);
                }
            } catch {}
        }
    }
}
