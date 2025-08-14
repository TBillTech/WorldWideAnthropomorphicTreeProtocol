// Node-only mock that mirrors the WebTransport adapter surface for tests.
// It reuses the Communication base and simulates a per-request bidirectional
// stream with async timing similar to the browser WebTransport adapter.
//
// Usage in tests:
//   const comm = new NodeWebTransportMock('mock://local');
//   comm.setMockHandler((request, data) => Uint8Array|Promise<Uint8Array>);
//   await comm.connect();
//   const sid = comm.getNewRequestStreamIdentifier(req);
//   await comm.sendRequest(sid, req, bytes);

import Communication from './communication.js';

export default class NodeWebTransportMock extends Communication {
    constructor(url = 'mock://local') {
        super();
        this.url = url;
        this._cid = `node-webtransport:${url}`;
        this._mockHandler = null; // fn(request: Request, data: Uint8Array) -> Uint8Array | Promise<Uint8Array>
    }

    connectionId() {
        return this._cid;
    }

    setMockHandler(fn) {
        this._mockHandler = fn;
    }

    async connect() {
        this._connected = true;
        // Simulate async ready like WebTransport.ready
        await Promise.resolve();
        return true;
    }

    async close() {
        this._connected = false;
        // Simulate async close
        await Promise.resolve();
        return true;
    }

    async sendRequest(sid, request, data = null, _options = {}) {
        if (!this._connected) throw new Error('Not connected');
        if (!this._mockHandler) throw new Error('No mock handler set');

        // Simulate creation of a bidi stream and async write/close/read phases
        const body = data instanceof Uint8Array ? data : (data ? new Uint8Array(data) : new Uint8Array());
        // microtask tick to emulate network scheduling
        await Promise.resolve();

        const respBytes = await this._mockHandler(request, body);

        // Another microtask tick to emulate response propagation
        await Promise.resolve();

        this._emitResponseEvent(sid, { type: 'response', ok: true, status: 200, data: respBytes });
        return { ok: true, status: 200 };
    }
}
