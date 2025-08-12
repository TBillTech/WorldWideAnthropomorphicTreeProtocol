import Communication from './communication.js';

// In-memory mock transport for tests. Allows registering a request handler that synchronously
// produces a response buffer for a given request.
export default class MockCommunication extends Communication {
    constructor() {
        super();
        this._cid = 'mock:local';
        this._connected = true;
        this._mockHandler = null; // fn(request: Request, data: Uint8Array) -> Uint8Array
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

    async sendRequest(sid, request, data = null, _options = {}) {
        if (!this._mockHandler) throw new Error('No mock handler set');
        const resp = await this._mockHandler(request, data || new Uint8Array());
        this._emitResponseEvent(sid, { type: 'response', ok: true, status: 200, data: resp });
        return { ok: true, status: 200 };
    }
}
