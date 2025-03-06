// filepath: /mnt/d/code/treebus/main.js
const QuicCommunication = require('./quic_communication');
const TcpCommunication = require('./tcp_communication');

function createCommunication(protocol) {
    if (protocol === 'QUIC') {
        return new QuicCommunication();
    } else if (protocol === 'TCP') {
        return new TcpCommunication();
    } else {
        throw new Error('Unsupported protocol');
    }
}

const protocol = 'QUIC'; // Or read from configuration
const communication = createCommunication(protocol);

communication.send('Hello, World!');
const response = communication.receive();
console.log('Received:', response);