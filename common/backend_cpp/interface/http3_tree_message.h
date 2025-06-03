#pragma once

#include <fplus/fplus.hpp>
#include "shared_chunk.h"
#include "tree_node.h"
#include "backend.h"

constexpr const string& get_signal_string(uint8_t signal);

using chunkList = std::list<shared_span<>>;

// The http3_tree_message is a psuedo-backend which kind of adheres to the backend.h interface
// for simplicity.  It is not a real backend, but rather a converter that converts the method
// calls into a chunks list, which can then be easily shipped as a request to the server.

// Each normal call is implemented as a pair of request and response methods on this class.
// The request methods initialize the class, and the response methods collect the result once the
// server has responded.

// These helper methods handle the tree main data types used by the class.
// The decoders return a maybe pair of the number of chunks consumed and the decoded data.
// If there are insufficient chunks, the decoder will return a maybe with no value.
// The can decode tests look forward by start_chunk chunks, 
// and check if there are enough chunks to decode the data at that chunk onward.
chunkList encode_label(uint16_t request_id, uint8_t signal, const string& str);
pair<size_t, string> decode_label(chunkList encoded);
fplus::maybe<size_t> can_decode_label(size_t start_chunk, chunkList encoded);

chunkList encode_long_string(uint16_t request_id, uint8_t signal, const string& str);
pair<size_t, string> decode_long_string(chunkList encoded);
fplus::maybe<size_t> can_decode_long_string(size_t start_chunk, chunkList encoded);

chunkList encode_MaybeTreeNode(uint16_t request_id, uint8_t signal, const fplus::maybe<TreeNode>& node);
pair<size_t, fplus::maybe<TreeNode> > decode_MaybeTreeNode(chunkList encoded);
fplus::maybe<size_t> can_decode_MaybeTreeNode(size_t start_chunk, chunkList encoded);

chunkList encode_SequentialNotification(uint16_t request_id, uint8_t signal, const SequentialNotification& notifications);
pair<size_t, SequentialNotification > decode_SequentialNotification(chunkList encoded);
fplus::maybe<size_t> can_decode_SequentialNotification(size_t start_chunk, chunkList encoded);

chunkList encode_VectorSequentialNotification(uint16_t request_id, uint8_t signal, const SequentialNotification& notifications);
pair<size_t, vector<SequentialNotification> > decode_VectorSequentialNotification(chunkList encoded);
fplus::maybe<size_t> can_decode_VectorSequentialNotification(size_t start_chunk, chunkList encoded);

chunkList encode_NewNodeVersion(uint16_t request_id, uint8_t signal, const NewNodeVersion& version);
pair<size_t, NewNodeVersion> decode_NewNodeVersion(chunkList encoded);
fplus::maybe<size_t> can_decode_NewNodeVersion(size_t start_chunk, chunkList encoded);

chunkList encode_SubTransaction(uint16_t request_id, uint8_t signal, const SubTransaction& transaction);
pair<size_t, SubTransaction> decode_SubTransaction(chunkList encoded);
fplus::maybe<size_t> can_decode_SubTransaction(size_t start_chunk, chunkList encoded);

chunkList encode_Transaction(uint16_t request_id, uint8_t signal, const Transaction& transaction);
pair<size_t, Transaction> decode_Transaction(chunkList encoded);
fplus::maybe<size_t> can_decode_Transaction(size_t start_chunk, chunkList encoded);

chunkList encode_VectorTreeNode(uint16_t request_id, uint8_t signal, const std::vector<TreeNode>& nodes);
pair<size_t, std::vector<TreeNode> > decode_VectorTreeNode(chunkList encoded);
fplus::maybe<size_t> can_decode_VectorTreeNode(size_t start_chunk, chunkList encoded);

class HTTP3TreeMessage {
public:
    HTTP3TreeMessage(uint16_t request_id = 0, uint8_t signal = 0) 
        : request_id_(request_id), signal_(signal) {
        // Constructor
    };
    ~HTTP3TreeMessage() = default;

    HTTP3TreeMessage(const HTTP3TreeMessage&) = delete; // No copy constructor
    HTTP3TreeMessage& operator=(const HTTP3TreeMessage&) = delete; // No copy assignment
    HTTP3TreeMessage(HTTP3TreeMessage&&) = default; // Move constructor

    // Tree Node format input and output methods
    // Each nominal backend method needs encode and docode for the request side,
    // and encode and decode for the response side.
    void encode_getNodeRequest(const std::string& label_rule);
    std::string decode_getNodeRequest();
    void encode_getNodeResponse(const fplus::maybe<TreeNode>& node);
    fplus::maybe<TreeNode> decode_getNodeResponse();

    void encode_upsertNodeRequest(const std::vector<TreeNode>& nodes);
    std::vector<TreeNode> decode_upsertNodeRequest();
    void encode_upsertNodeResponse(bool success);
    bool decode_upsertNodeResponse();
    
    void encode_deleteNodeRequest(const std::string& label_rule);
    std::string decode_deleteNodeRequest();
    void encode_deleteNodeResponse(bool success);
    bool decode_deleteNodeResponse();
    
    void encode_getPageTreeRequest(const std::string& page_node_label_rule);
    std::string decode_getPageTreeRequest();
    void encode_getPageTreeResponse(const std::vector<TreeNode>& nodes);
    std::vector<TreeNode> decode_getPageTreeResponse();
    
    void encode_getQueryNodesRequest(const std::string& label_rule);
    std::string decode_getQueryNodesRequest();
    void encode_getQueryNodesResponse(const std::vector<TreeNode>& nodes);
    std::vector<TreeNode> decode_getQueryNodesResponse();
    
    void encode_openTransactionLayerRequest(const TreeNode& node);
    TreeNode decode_openTransactionLayerRequest();
    void encode_openTransactionLayerResponse(bool success);
    bool decode_openTransactionLayerResponse();
    
    void encode_closeTransactionLayersRequest();
    void decode_closeTransactionLayersRequest() {};
    void encode_closeTransactionLayersResponse(bool success);
    bool decode_closeTransactionLayersResponse();
    
    void encode_applyTransactionRequest(const Transaction& transaction);
    Transaction decode_applyTransactionRequest();
    void encode_applyTransactionResponse(bool success);
    bool decode_applyTransactionResponse();
    
    void encode_getFullTreeRequest();
    void decode_getFullTreeRequest(){};
    void encode_getFullTreeResponse(const std::vector<TreeNode>& nodes);
    std::vector<TreeNode> decode_getFullTreeResponse();
    
    void encode_registerNodeListenerRequest(const std::string& listener_name, const std::string& label_rule, bool child_notify);
    tuple<std::string, std::string, bool> decode_registerNodeListenerRequest();
    void encode_registerNodeListenerResponse(bool success);
    bool decode_registerNodeListenerResponse();

    void encode_deregisterNodeListenerRequest(const std::string& listener_name, const std::string& label_rule);
    pair<std::string, std::string> decode_deregisterNodeListenerRequest();
    void encode_deregisterNodeListenerResponse(bool success);
    bool decode_deregisterNodeListenerResponse();

    void encode_notifyListenersRequest(const std::string& label_rule, const fplus::maybe<TreeNode>& node);
    pair<std::string, fplus::maybe<TreeNode> > decode_notifyListenersRequest();
    void encode_notifyListenersResponse(bool success);
    bool decode_notifyListenersResponse();

    void encode_processNotificationRequest();
    void decode_processNotificationRequest() {};
    void encode_processNotificationResponse();
    void decode_processNotificationResponse() {};
    
    void encode_getJournalRequest(SequentialNotification const& last_notification);
    SequentialNotification decode_getJournalRequest();
    void encode_getJournalResponse(const std::vector<SequentialNotification>& notifications);
    std::vector<SequentialNotification> decode_getJournalResponse();

    void setup_staticNodeDataRequest();
    // staticNodeDataRequest is a special case which has no explicit encoding or decoding

    // Controlling message request and response state methods
    bool isInitialized() const {
        return isInitialized_;
    }
    void setRequestId(uint16_t request_id); // Implicitly sets request complete
    bool isRequestComplete() const {
        return requestComplete;
    }
    bool isResponseComplete() const {
        return responseComplete;
    }
    bool isProcessingFinished() const {
        return processingFinished;
    }

    // Chunk input and output methods
    fplus::maybe<shared_span<> > popRequestChunk();
    void pushRequestChunk(shared_span<> chunk);

    fplus::maybe<shared_span<> > popResponseChunk();
    void pushResponseChunk(shared_span<> chunk);

    void reset();

    uint8_t getSignal() const {
        return signal_;
    }
 
    // assignment operator
    HTTP3TreeMessage(const HTTP3TreeMessage&& other) :
        request_id_(other.request_id_),
        signal_(other.signal_),
        isInitialized_(other.isInitialized_),
        requestChunks(std::move(other.requestChunks)),
        requestComplete(other.requestComplete),
        responseChunks(std::move(other.responseChunks)),
        responseComplete(other.responseComplete),
        processingFinished(other.processingFinished) {
    }

private:
    // loosely, the process is broken down into stages:
    // 1. The class is constructed unitialized
    // 2. A method is selected and called to initialize the class data and state
    // 3. The Http3ClientBackendUpdater maintainRequestHandlers provides a stream identifier, 
    //    the chunkList for the wire a produced, and the handler is registered with the quic connector 
    // 4. The handler consumes the request chunkList and sends them to the server
    // 5. The handler appends response chunkList until the response is complete
    // 6. The HTTP3ClientBackend processFinishedRequest method is called, which will call the appropriate response method
    uint16_t request_id_;
    uint8_t signal_;

    bool isInitialized_ = false;
    chunkList requestChunks;

    bool requestComplete = false;
    chunkList responseChunks;

    bool responseComplete = false;

    bool processingFinished = false;

    std::mutex requestChunksMutex;
    std::mutex responseChunksMutex;
};