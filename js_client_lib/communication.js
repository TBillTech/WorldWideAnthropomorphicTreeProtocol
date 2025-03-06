// filepath: /mnt/d/code/treebus/communication.js
class Communication {
    send(data) {
        throw new Error('send() must be implemented');
    }

    receive() {
        throw new Error('receive() must be implemented');
    }
}

module.exports = Communication;