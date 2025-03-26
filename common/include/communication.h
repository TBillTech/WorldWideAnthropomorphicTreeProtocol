#pragma once
#include <string>
#include <cstring> // Add this line to include the memcmp function
#include <vector>
#include <functional>
#include <cstdint>
#include <iostream>
#include <map>
#include <span>
#include <utility>
#include <ngtcp2/ngtcp2.h> // Add this line to include the ngtcp2_cid type

#include "config_base.h"
#include "shared_chunk.h"

using namespace std;

// Server and Client Connection IDs:
//
// The server primarily uses the dcid (Destination Connection ID) to identify incoming connections.
// The client primarily uses the scid (Source Connection ID) to identify itself to the server.
//
// Handler Objects:
//
// There is one Handler object per dcid. The Handler object manages the overall connection associated with that dcid.
//
// Stream Objects:
//
// There is a Stream object for each stream_id within a connection. Each Handler object manages multiple Stream objects, one for each active stream within the connection.
//
// Here is a summary of how these components interact:
//
// When a new connection is initiated, a Handler object is created and associated with the dcid.
// The Handler object manages the connection and handles incoming packets, delegating stream-specific tasks to Stream objects.
// Each Stream object is responsible for handling the data and state associated with a specific stream within the connection.
//
// This design allows the server to efficiently manage multiple connections and streams, with each Handler object managing a single connection and each Stream object managing a single stream within that connection.

struct StreamIdentifier {
    bool is_internal;
    bool is_idle;
    // cid is not needed for client side, since the client will know the scid already, 
    // but is important for server side to identify the handler object associated with the connection.
    ngtcp2_cid cid; 
    // stream_id identifies which stream in the connection defines this request;
    // consecutive even numbers for client requests, consecutive odd numbers for server pushes (legacy)
    uint64_t stream_id;

    bool operator<(const StreamIdentifier& other) const {
        using namespace ngtcp2;
        if (cid < other.cid) {
            return true;
        }
        if (other.cid < cid) {
            return false;
        }
        if (stream_id < other.stream_id) {
            return true;
        }
        if (stream_id > other.stream_id) {
            return false;
        }
        if (is_internal != other.is_internal) {
            return is_internal < other.is_internal;
        }
        return is_idle < other.is_idle;
    }

    bool operator==(const StreamIdentifier& other) const {
        using namespace ngtcp2;
        return is_internal == other.is_internal && is_idle == other.is_idle && cid == other.cid && stream_id == other.stream_id;
    }

    bool isInternal() const {
        // To be internal, it is enough that the port number is 0, since there is no realistic use case for port 0
        return is_internal;
    }

    bool isIdle() const {
        return is_idle;
    }

    friend ostream& operator<<(ostream& os, const StreamIdentifier& si) {
        // If the flags are set, then print them and ignore the ids:
        if (si.is_internal) {
            os << "Internal StreamIdentifier: is_internal=" << si.is_internal << ", is_idle=" << si.is_idle;
            return os;
        }
        // Otherwise, ignore the flags and print the ids:
        using namespace ngtcp2;
        os << "StreamIdentifier: cid=" << si.cid << ", stream_id=" << si.stream_id;
        return os;
    }
};

typedef vector<shared_span<> > chunks;

// The stream callback function is generally used by both sides to process incoming and outgoing data for bidirectional streams.
using stream_callback_fn = function<chunks(const StreamIdentifier&, chunks&)>;

// On Client side, the initial outgoing request is first logged into the requestResolutionQueue (because it has no response callbacks yet)
// And because the StreamIdentifier is not yet known.
typedef pair<Request, stream_callback_fn> RequestCallback;
// RequestCallback Vector needs to sort by the Request object, so redirect < operator on RequestCallback to Request:
inline bool operator<(RequestCallback& lhs, RequestCallback& rhs) {
    return lhs.first < rhs.first;
}
typedef vector<RequestCallback> request_resolutions;

// On Server side, the incoming requests are first logged into the unhandled_response_queue (because it has no response callbacks yet)
// Normally, the communication object will query the QUIC server object to get more information about the request.
typedef vector<pair<StreamIdentifier, Request>> unhandled_response_queue;

// And for processing of the stream data, the StreamIdentifier is attached to a callback function
typedef pair<StreamIdentifier, stream_callback_fn> stream_callback;

// While a round robin queue for load balancing is done (for now) with a simple vector.
typedef vector<stream_callback> stream_callbacks;

// Also, it is necessary to map StreamIdentifiers to actually received/sent data chunks to the stream for it.
// And prepared but not yet sent-on-the-wire data chunks can use the same mapping.
typedef map<StreamIdentifier, chunks> stream_data_chunks;

// There are some special case internal identified_request objects which encode non-network logic:
// The IDLE_identified_request is used to indicate that the requestor_callback_streams is empty.
extern StreamIdentifier IDLE_stream;
extern pair<StreamIdentifier, Request> IDLE_stream_listener;

class Communication {
public:
    // ## "client" side communication model: worker threads + HTTP3 IO thread
    // worker thread gets a StreamIdentifier from the Communication object
    // worker thread calls setupRequestStream, which adds the request to the requestSentQueue, inserts the bytes in the "socket" send buffer.
    // HTTP3 IO thread flushes bytes from the "socket" send buffer to the wire.
    // HTTP3 IO thread receives bytes from the wire, correlates them with the request id, pops from the requestSentQueue, and adds the response to the responseReceiveQueue.
    // worker thread calls processRequestStream, which pops the response from the responseReceiveQueue and calls the callback function.
    // Thus, the two public methods that the worker thread needs are setupRequestStream and processRequestStream.
    // The HTTP3 IO thread is an instance of this class, so does not explicitly need any public methods.  (but maybe private methods)
    // A new request requires a new StreamIdentifier, which the Communication object knows how to create based on URI and other info.
    virtual void resolveRequestStream(Request const &req, stream_callback_fn cb) = 0;
    // In order to separate the nominally client side handler threads from the Communication IO thread, a handler thread can call 
    // this function to make the callback from a worker thread and take as much time as necessary without interrupting the 
    // Communication IO.  Returns false if idle.
    virtual bool processRequestStream() = 0;

    // ## "server" side communication model
    // HTTP3 IO thread receives request bytes from the wire, and adds them to the requestReceiveQueue.
    // worker thread calls listenForResponseStream, which pops the request from the requestReceiveQueue, returns the request and request_id.
    // worker thread finishes computing the appropriate response, and calls processResponseStream, which inserts the bytes int the "socket" send buffer.
    // HTTP3 IO thread flushes bytes from the "socket" send buffer to the wire.
    // Thus, the two public methods that the worker thread needs are listenForResponseStream and processResponseStream.

    // The server (and sometimes the client) can receive a request on the wire, and then add this request to
    // to the requestReceiveQueue.  To get the next request, the worker thread calls listenForResponseStream.
    // For example, the server receives a request for asset Box1, and then adds this request to the requestReceiveQueue.
    // There is no callback argument here, because sending the response asynchronously copies to the "socket" send queue.
    virtual pair<StreamIdentifier, Request> listenForResponseStream() = 0;
    // The server (and sometimes the client) can will then set up a response handler/callback for creating data chunks to send.
    virtual void setupResponseStream(stream_callback& response) = 0;
    // The server (and sometimes client) can send a response asynchronously via this Communication class which does not queue.
    // For example, server send asset Box1 asynchronously.  Returns false if idle.
    virtual bool processResponseStream() = 0;

    // The Protocol facing side of the algorithm runs in a separate thread to the above methods, which are for the data users.
    // The following methods are suggested for the protocol facing side of the algorithm, and are called by the protocol facing thread.
    // setupRequests and setupResponses drain the requestResolutionQueue and fill the unhandledQueue.
    // virtual void setupRequests() = 0;
    // virtual void setupResponses() = 0;
    // send and receive deal with data coming in and going out on the streams
    // virtual void send() = 0;
    // virtual void receive() = 0;

    // Listen is for servicing the protocol server side.
    virtual void listen(const string &local_name, const string& local_ip_addr, int local_port) = 0;
    // Close is for shutting down the protocol thread.
    virtual void close() = 0;
    // Connect is for servicing the protocol client side.
    virtual void connect(const string &peer_name, const string& peer_ip_addr, int peer_port) = 0;
    virtual ~Communication() = default;
};