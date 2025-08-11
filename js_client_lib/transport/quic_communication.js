import Communication from './communication.js';

// Node-only placeholder. Do not include in browser bundles. Implementations should be guarded
// by environment checks and only used in Node test environments.
export default class QuicCommunication extends Communication {
    constructor() {
        super();
        this._unavailable = true;
    }

    send() {
        throw new Error('QuicCommunication is a Node-only placeholder and not available in browser.');
    }

    receive() {
        throw new Error('QuicCommunication is a Node-only placeholder and not available in browser.');
    }
}