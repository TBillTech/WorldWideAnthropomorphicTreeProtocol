export default class Communication {
    // Browser-first abstract adapter. Concrete implementations should avoid Node-only APIs.
    send(_data) {
        throw new Error('send() must be implemented');
    }

    receive() {
        throw new Error('receive() must be implemented');
    }
}