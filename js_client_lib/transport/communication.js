// Abstract transport adapter. Browser-first design; avoid Node-only APIs in concrete subclasses.
// Responsibilities (aligned with C++ Communication):
// - Stream identifiers per logical request
// - Response handler registration & routing
// - Optional request handler hooks (primarily for mocks/tests)
// - Connection lifecycle: connect(), listen(), close()
// - Processing helpers for request/response streams (no-ops by default)
import StreamIdentifier from './stream_identifier.js';

export default class Communication {
    constructor() {
        this._responseHandlers = new Map(); // key: StreamIdentifier.toString() -> (event) => void
        this._requestHandlers = new Map(); // name -> prepare_fn
        this._sidCounter = 0;
        this._connected = false;
    }

    // Returns a new StreamIdentifier for a given Request. Override connectionId() to provide stable cid.
    getNewRequestStreamIdentifier(_request) {
        const cid = this.connectionId();
        const logicalId = ++this._sidCounter;
        return new StreamIdentifier(cid, logicalId);
    }

    // Override to return a stable connection identifier string for this adapter/endpoint.
    connectionId() {
        throw new Error('connectionId() must be implemented by subclass');
    }

    registerResponseHandler(sid, cb) {
        this._responseHandlers.set(String(sid), cb);
    }

    hasResponseHandler(sid) {
        return this._responseHandlers.has(String(sid));
    }

    deregisterResponseHandler(sid) {
        this._responseHandlers.delete(String(sid));
    }

    registerRequestHandler(name, prepareFn) {
        this._requestHandlers.set(name, prepareFn);
    }

    deregisterRequestHandler(name) {
        this._requestHandlers.delete(name);
    }

    // Default no-ops; concrete adapters can implement polling/looping if needed.
    processRequestStream() {
        return false;
    }

    processResponseStream() {
        return false;
    }

    // Lifecycle. listen() primarily for server/mocks; connect() for clients.
    async listen() {
        throw new Error('listen() must be implemented by subclass');
    }

    async connect() {
        throw new Error('connect() must be implemented by subclass');
    }

    async close() {
        throw new Error('close() must be implemented by subclass');
    }

    get isConnected() {
        return this._connected;
    }

    // Core send API: subclasses must implement. `data` may be Uint8Array, ArrayBuffer,
    // ReadableStream<Uint8Array>, or AsyncIterable<Uint8Array>.
    async sendRequest(_sid, _request, _data = null, _options = {}) {
        throw new Error('sendRequest() must be implemented by subclass');
    }

    // Helper for adapters to deliver events to registered response handlers.
    _emitResponseEvent(sid, event) {
        const key = String(sid);
        const cb = this._responseHandlers.get(key);
        if (cb) cb(event);
    }
}