// Fetch-based transport adapter. In modern browsers, the UA may negotiate HTTP/3 (QUIC)
// transparently for fetch requests to https origins that support it. While fetch doesn't
// expose QUIC streams directly, this adapter provides a simple request/response path
// suitable for non-streaming WWATP operations.
// For streaming, consider WebTransport (see webtransport_communication.js).

import Communication from './communication.js';

export default class FetchCommunication extends Communication {
    constructor(baseUrl) {
        super();
        this.baseUrl = baseUrl.replace(/\/$/, '');
        this._connected = true; // fetch requires no persistent connection
        this._cid = `fetch:${this.baseUrl}`;
    }

    connectionId() {
        return this._cid;
    }

    async connect() {
        this._connected = true;
        return true;
    }

    async close() {
        this._connected = false;
        return true;
    }

    // Send a single request; `request.path` is appended to baseUrl.
    // data: Uint8Array | ArrayBuffer | null
    async sendRequest(sid, request, data = null, options = {}) {
        const url = this.baseUrl + request.path;
        const controller = new AbortController();
        const headers = new Headers(options.headers || {});
        if (data && !headers.has('content-type')) {
            headers.set('content-type', 'application/octet-stream');
        }

        const response = await fetch(url, {
            method: request.method || 'POST',
            body: data ? (data.buffer ? data : new Uint8Array(data)) : null,
            headers,
            signal: controller.signal,
        });

        const buf = new Uint8Array(await response.arrayBuffer());
        this._emitResponseEvent(sid, { type: 'response', ok: response.ok, status: response.status, data: buf });
        return { ok: response.ok, status: response.status };
    }
}
