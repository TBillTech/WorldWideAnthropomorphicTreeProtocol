// WebTransport-based transport adapter (browser-only). Requires secure context and server support.
// Exposes a simple sendRequest API over a new bidirectional stream per request.
// Note: WebTransport API availability varies; callers should feature-detect.

import Communication from './communication.js';

export default class WebTransportCommunication extends Communication {
    constructor(url) {
        super();
        this.url = url;
        this.transport = null;
        this._cid = `webtransport:${url}`;
    }

    connectionId() {
        return this._cid;
    }

    async connect() {
        if (typeof WebTransport === 'undefined') {
            throw new Error('WebTransport is not available in this environment');
        }
        this.transport = new WebTransport(this.url);
        await this.transport.ready;
        this._connected = true;
        this.transport.closed.closed.then(() => {
            this._connected = false;
        }).catch(() => {
            this._connected = false;
        });
        return true;
    }

    async close() {
        if (this.transport) {
            await this.transport.close();
        }
        this._connected = false;
        return true;
    }

    async sendRequest(sid, _request, data = null, _options = {}) {
        if (!this.transport) throw new Error('Not connected');
        const bidi = await this.transport.createBidirectionalStream();
        const writer = bidi.writable.getWriter();
        if (data) await writer.write(data);
        await writer.close();
        const reader = bidi.readable.getReader();
        const chunks = [];
        while (true) {
            const { value, done } = await reader.read();
            if (done) break;
            if (value) chunks.push(value);
        }
        const totalLen = chunks.reduce((a, c) => a + c.byteLength, 0);
        const out = new Uint8Array(totalLen);
        let off = 0;
        for (const c of chunks) {
            out.set(new Uint8Array(c), off);
            off += c.byteLength;
        }
        this._emitResponseEvent(sid, { type: 'response', ok: true, status: 200, data: out });
        return { ok: true, status: 200 };
    }
}
