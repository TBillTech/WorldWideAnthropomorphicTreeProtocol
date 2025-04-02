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

class StreamIdentifier {
    public:
    StreamIdentifier(ngtcp2_cid cid, int64_t stream_id) : cid(cid), stream_id(stream_id) {};
    StreamIdentifier(StreamIdentifier const &other) : cid(other.cid), stream_id(other.stream_id) {};
        
    // cid is not needed for client side, since the client will know the scid already, 
    // but is important for server side to identify the handler object associated with the connection.
    ngtcp2_cid cid; 
    // stream_id identifies which stream in the connection defines this request;
    // consecutive even numbers for client requests, consecutive odd numbers for server pushes (legacy)
    int64_t stream_id;


    bool operator<(const StreamIdentifier& other) const {
        using namespace ngtcp2;
        if (cid < other.cid) {
            return true;
        }
        if (other.cid < cid) {
            return false;
        }
        return stream_id < other.stream_id;
    }

    bool operator==(const StreamIdentifier& other) const {
        using namespace ngtcp2;
        return cid == other.cid && stream_id == other.stream_id;
    }

    friend ostream& operator<<(ostream& os, const StreamIdentifier& si) {
        using namespace ngtcp2;
        os << "StreamIdentifier: cid=" << si.cid << ", stream_id=" << si.stream_id;
        return os;
    }
};

typedef vector<shared_span<> > chunks;

struct generic_signal {
    // generic_signal is not really decodec yet, so is in fact just a shared_span reference
    generic_signal(shared_span<> &span) : signal_type(span.get_signal()), signal(span) {}
    generic_signal(generic_signal const &other) :
        signal_type(other.signal_type), signal(other.signal) {}
    uint64_t signal_type;
    shared_span<> const &signal;
};

struct pod_signal {
    // pod_signal constructor from shared_span interprets the shared_span data as a pod_signal copy of itself
    pod_signal(const shared_span<> &span) {
        auto it = span.begin<pod_signal>();
        auto other = *it;
        signal = other.signal;
        signal_code = other.signal_code;
    }
    pod_signal(uint64_t signal_value, uint64_t signal_code) : signal(signal_value), signal_code(signal_code) {}
    pod_signal(pod_signal const &other) :
        signal(other.signal), signal_code(other.signal_code) {}
    uint64_t signal;
    uint64_t signal_code;
    static constexpr uint64_t GLOBAL_SIGNAL_TYPE = 1; 
    static constexpr uint64_t SIGNAL_CLOSE_STREAM = 0x00000001;
    static constexpr uint64_t SIGNAL_HEARTBEAT = 0x00000002;
};

// This is a helper function that looks at the shared_span signal value, and returns a pod_signal object if and only if the signal value is 1.
// This should be compatible with templated signal decode functions that operate with different return types if the signal value is higher than 1.
template<typename SIGNAL_TYPE>
inline SIGNAL_TYPE get_signal(const shared_span<> &span) {
    // non template specilializations return the generic_signal
    return generic_signal(span);
}

template<>
inline pod_signal get_signal<pod_signal>(const shared_span<> &span) {
    // template specialization returns the pod_signal
    return pod_signal(span);
}

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

// And for processing of the stream data, the StreamIdentifier is attached to a callback function
typedef pair<StreamIdentifier, stream_callback_fn> stream_callback;
struct uri_response_info {
    bool can_handle;
    bool is_live_stream;
    bool is_file;
    size_t dyn_length;
};

typedef pair<uri_response_info, stream_callback> prepared_stream_callback;

// While a round robin queue for load balancing is done (for now) with a simple vector.
typedef vector<stream_callback> stream_callbacks;

// Also, it is necessary to map StreamIdentifiers to actually received/sent data chunks to the stream for it.
// And prepared but not yet sent-on-the-wire data chunks can use the same mapping.
typedef map<StreamIdentifier, chunks> stream_data_chunks;

// Define a callback function type for preparing stream callbacks
using prepare_stream_callback_fn = function<prepared_stream_callback(const StreamIdentifier&, const std::string_view &)>;
typedef pair<string, prepare_stream_callback_fn> named_prepare_fn;
typedef vector<named_prepare_fn> named_prepare_fns;

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

    // The server (and sometimes the client) can receive a request on the wire, and when it does, it needs to immediately 
    // handle the incoming request by deciding if it is a normal file request, or maybe the wwatp protocol (for example).
    // To do this, the user code needs to register a callback function to handle the request (which will be called from the protocol thread).
    virtual void registerRequestHandler(named_prepare_fn preparer) = 0;
    virtual void deregisterRequestHandler(string preparer_name) = 0;
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