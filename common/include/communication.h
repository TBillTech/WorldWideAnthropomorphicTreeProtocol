#pragma once
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

using namespace std;

struct RequestIdentifier {
    string cid;
    uint16_t port;
    string local_addr;
    string peer_addr;
    // consecutive even numbers for client requests, consecutive odd numbers for server pushes (legacy)
    uint64_t stream_id;

    bool operator==(const RequestIdentifier& other) const {
        return cid == other.cid && port == other.port && local_addr == other.local_addr && peer_addr == other.peer_addr && stream_id == other.stream_id;
    }

    bool isInternal() const {
        // To be internal, it is enough that the port number is 0, since there is no realistic use case for port 0
        return port == 0;
    }

    bool isIdle() const {
        return isInternal() && cid == "IDLE";
    }
};

// Use c++ lambda functions to define a requestor callback vector pair.  
// The callback function is a lambda function that takes the response message string, 
// the request id, and the RequestResponseStack object as inputs 
// and returns void.
typedef pair<RequestIdentifier, string> identified_request;
typedef vector<identified_request> identified_request_vector;
typedef vector<pair<identified_request, string>> identified_response_vector;
using requestor_callback_fn = function<void(const identified_request&, const string&)>;
typedef pair<identified_request, requestor_callback_fn> requestor_callback;
typedef vector<requestor_callback> requestor_callback_vector;
typedef pair<requestor_callback, string> completed_callback;
typedef vector<completed_callback> completed_callback_vector;

// There are some special case internal identified_request objects which encode non-network logic:
// The IDLE_identified_request is used to indicate that the requestor_callback_vector is empty.
static identified_request IDLE_identified_request = make_pair(RequestIdentifier{"IDLE", 0, "", "", 0}, "");

class Communication {
public:
    // ## "client" side communication model: worker threads + HTTP3 IO thread
    // worker thread gets a RequestIdentifier from the Communication object
    // worker thread calls requestSend, which adds the request to the requestSentQueue, inserts the bytes in the "socket" send buffer.
    // HTTP3 IO thread flushes bytes from the "socket" send buffer to the wire.
    // HTTP3 IO thread receives bytes from the wire, correlates them with the request id, pops from the requestSentQueue, and adds the response to the responseReceiveQueue.
    // worker thread calls processNextResponse, which pops the response from the responseReceiveQueue and calls the callback function.
    // Thus, the two public methods that the worker thread needs are requestSend and processNextResponse.
    // The HTTP3 IO thread is an instance of this class, so does not explicitly need any public methods.  (but maybe private methods)

    // A new request requires a new RequestIdentifier, which the Communication object knows how to create.
    virtual RequestIdentifier getNextRequestIdentifier() = 0;
    // The client (and sometimes server) can send a request asynchronously via this Communication class and add
    // the request to the requestQueque.
    // For example, client requests asset Box1, and when response comes, callback function will handle it. 
    virtual void requestSend(const requestor_callback& request) = 0;
    // In order to separate the nominally client side handler threads from the Communication IO thread, a handler thread can call 
    // this function to make the callback from a worker thread and take as much time as necessary without interrupting the 
    // Communication IO.
    virtual void processNextResponse() = 0;

    // ## "server" side communication model
    // HTTP3 IO thread receives request bytes from the wire, and adds them to the requestReceiveQueue.
    // worker thread calls requestReceive, which pops the request from the requestReceiveQueue, returns the request and request_id.
    // worker thread finishes computing the appropriate response, and calls responseSend, which inserts the bytes int the "socket" send buffer.
    // HTTP3 IO thread flushes bytes from the "socket" send buffer to the wire.
    // Thus, the two public methods that the worker thread needs are requestReceive and responseSend.

    // The server (and sometimes the client) can receive a request on the wire, and then add this request to
    // to the requestReceiveQueue.  To get the next request, the worker thread calls requestReceive.
    // For example, the server receives a request for asset Box1, and then adds this request to the requestReceiveQueue.
    // There is no callback argument here, because sending the response asynchronously copies to the "socket" send queue.
    virtual identified_request requestReceive() = 0;
    // The server (and sometimes client) can send a response asynchronously via this Communication class which does not queue.
    // For example, server send asset Box1 asynchronously. 
    virtual void responseSend(const identified_request& request, const string& response) = 0;
    virtual void listen(const string &local_name, const string& local_ip_addr, int local_port) = 0;
    virtual void close() = 0;
    virtual void connect(const string &peer_name, const string& peer_ip_addr, int peer_port) = 0;
    virtual ~Communication() = default;
};