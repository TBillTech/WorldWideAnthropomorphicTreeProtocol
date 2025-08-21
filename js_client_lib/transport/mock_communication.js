import Communication from './communication.js';

// In-memory mock transport for tests. Allows registering a request handler that synchronously
// produces a response buffer for a given request.
export default class MockCommunication extends Communication {
    constructor() {
        super();
        this._cid = 'mock:local';
        this._connected = true;
        this._mockHandler = null; // fn(request: Request, data: Uint8Array) -> Uint8Array
    // Accumulate per-chunk writes for the same sid and flush once per tick
    // Map key: sid object -> { parts: Uint8Array[], timer: Timeout }
    this._pending = new Map();
    }

    connectionId() {
        return this._cid;
    }

    setMockHandler(fn) {
        this._mockHandler = fn;
    }

    async connect() {
        this._connected = true;
        return true;
    }

    async close() {
        this._connected = false;
        return true;
    }

    async sendRequest(sid, request, data = null, options = {}) {
        if (!this._mockHandler) throw new Error('No mock handler set');
        const key = sid; // use object identity for per-request aggregation
        const buf = data instanceof Uint8Array ? data : (data ? new Uint8Array(data) : new Uint8Array());
        let ent = this._pending.get(key);
        if (!ent) { ent = { parts: [], scheduled: false }; this._pending.set(key, ent); }
        ent.parts.push(buf);

        const reqSig = options?.requestSignal;
        const REQUEST_FINAL = 0x05;

        const flush = async () => {
            try {
                const cur = this._pending.get(key);
                if (!cur) return;
                this._pending.delete(key);
                const total = cur.parts.reduce((s, p) => s + (p?.byteLength || 0), 0);
                const out = new Uint8Array(total);
                let o = 0; for (const p of cur.parts) { if (p && p.byteLength) { out.set(p, o); o += p.byteLength; } }
                const resp = await this._mockHandler(request, out);
                this._emitResponseEvent(sid, { type: 'response', ok: true, status: 200, data: resp });
            } catch (_) {
                // Best-effort; ignore handler errors here to keep tests moving
            }
        };

        // For non-WWATP or REQUEST_FINAL, flush immediately
        if (reqSig === undefined || reqSig === REQUEST_FINAL) {
            await flush();
        } else {
            // For WWATP multi-chunk, schedule a microtask flush if not already scheduled
            if (!ent.scheduled) {
                ent.scheduled = true;
                queueMicrotask(() => {
                    ent.scheduled = false;
                    flush();
                });
            }
        }
        return { ok: true, status: 200 };
    }
}
