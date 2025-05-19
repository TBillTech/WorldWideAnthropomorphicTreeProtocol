#pragma once

#include "backend.h"
#include "communication.h"
#include "http3_tree_message.h"
#include "request.h"

// The journaling feature interfaces with the http3 server to request the latest list of notifications beyond the client's current.
// Moreover, since the server does not push notifications, it will replay notifications up to some past limit whenever the client desires.

class Http3ClientBackend : public Backend {
public:
    // Make sure blocking_mode is set to true if you want to block on the applyTransaction call, 
    // ESPECIALLY if you wrap this class in a TransactionalBackend.
    Http3ClientBackend(Backend& local_backend, bool blocking_mode, 
                       Request request)
        : localBackend_(local_backend), lastNotification_({0, {}}), 
          blockingMode_(blocking_mode), requestUrlInfo_(request) {};
    ~Http3ClientBackend() override = default;

    Http3ClientBackend(const Http3ClientBackend&& other) :
        localBackend_(other.localBackend_), lastNotification_(other.lastNotification_),
        blockingMode_(other.blockingMode_), requestUrlInfo_(other.requestUrlInfo_) {};
    // Delete copy constructor
    Http3ClientBackend& operator=(const Http3ClientBackend&) = delete;

    // Retrieve a node by its label rule.
    fplus::maybe<TreeNode> getNode(const std::string& label_rule) const override;

    // Add or update a parent node and its children in the tree.
    bool upsertNode(const std::vector<TreeNode>& nodes) override;

    // Delete a node and its children from the tree.
    bool deleteNode(const std::string& label_rule) override;

    std::vector<TreeNode> getPageTree(const std::string& page_node_label_rule) const override;
    std::vector<TreeNode> relativeGetPageTree(const TreeNode& node, const std::string& page_node_label_rule) const override;

    // Query nodes matching a label rule.
    std::vector<TreeNode> queryNodes(const std::string& label_rule) const override;
    std::vector<TreeNode> relativeQueryNodes(const TreeNode& node, const std::string& label_rule) const override;

    // openTransactionLayer and closeTransactionLayers are passed through to the local backend, which is probably not transactional.
    // For the ability to truly do transactions correctly with the server, the transaction layer should be ON TOP of this Http3ClientBackend.
    // The Http3ClientBackend will then be responsible for sending the transaction to the server, and journaling changes.
    bool openTransactionLayer(const TreeNode& node) override;
    bool closeTransactionLayers(void) override;
    // But applyTransaction will block, _waiting for the server to respond_ if this class is in blocking mode.
    bool applyTransaction(const Transaction& transaction) override;

    // Retrieve the entire tree structure (for debugging or full sync purposes).
    std::vector<TreeNode> getFullTree() const override;

    void registerNodeListener(const std::string listener_name, const std::string label_rule, bool child_notify, NodeListenerCallback callback) override;
    void deregisterNodeListener(const std::string listener_name, const std::string label_rule) override;

    // Notify listeners for a specific label rule.
    void notifyListeners(const std::string& label_rule, const fplus::maybe<TreeNode>& node) override;

    void processNotification() override {};

    // Processing requests and responses are outside the backend interface.  
    // This is because worker threads need to be able to
    // synchronize the localBackend_ with the server and the journal.
    void processHTTP3Response(HTTP3TreeMessage& response); 

    bool hasNextRequest() const {
        std::lock_guard<std::mutex> lock(handlerMutex_);
        return !pendingRequests_.empty();
    }
    HTTP3TreeMessage popNextRequest(); 
    HTTP3TreeMessage solicitJournalRequest() const;
    const Request getRequestUrlInfo() const { return requestUrlInfo_; }

    void requestFullTreeSync();
    void setMutablePageTreeLabelRule(std::string label_rule = "MutableNodes") {
        std::lock_guard<std::mutex> lock(handlerMutex_);
        mutablePageTreeLabelRule_ = label_rule;
    }
    void getMutablePageTree();

private:
    void getMutablePageTreeInMode(bool blocking_mode);

    void awaitResponse();
    bool awaitBoolResponse();
    fplus::maybe<TreeNode> awaitTreeResponse();
    std::vector<TreeNode> awaitVectorTreeResponse();

    void responseSignal();

    void processOneNotification(SequentialNotification const& notification);
    void updateLastNotification(SequentialNotification const& notifications);

    void disableServerSync() {
        std::lock_guard<std::mutex> lock(handlerMutex_);
        serverSyncOn_ = false;
    }
    void enableServerSync() {
        std::lock_guard<std::mutex> lock(handlerMutex_);
        serverSyncOn_ = true;
    }

    mutable std::mutex backendMutex_;
    Backend& localBackend_;
    mutable bool needMutablePageTreeSync_ = false;

    // Mutex to lock the handler requests
    mutable std::mutex handlerMutex_;
    // The client tracks the last notification in order to keep up with changes on the server.
    // id == 0, empty means client need to completely reload the mutable part of the tree (at least).
    // The client does NOT need to keep the full journal of notifications
    SequentialNotification lastNotification_;
    string mutablePageTreeLabelRule_ = "MutableNodes";
    mutable bool serverSyncOn_ = true;

    bool blockingMode_;
    std::mutex blockingMutex_;
    std::condition_variable responseCondition;
    mutable bool responseReady = false;
    bool boolResponse = false;
    fplus::maybe<TreeNode> maybeTreeResponse_;
    std::vector<TreeNode> vectorTreeResponse_;

    const Request requestUrlInfo_;
    
    std::list<HTTP3TreeMessage> pendingRequests_;
};

class Communication;

class Http3ClientBackendUpdater {
    public:
        Http3ClientBackendUpdater(size_t journalRequestsPerMinute = 60) 
            : journalRequestsPerMinute_(journalRequestsPerMinute) {};
        ~Http3ClientBackendUpdater() = default;

        // Delete copy constructor and copy assignment operator
        Http3ClientBackendUpdater(const Http3ClientBackendUpdater& other) = delete;
        Http3ClientBackendUpdater& operator=(const Http3ClientBackendUpdater&) = delete;

        // Add a backend to the updater. This method should only be used with Request object that are tree urls.
        // Getting static assets at Request urls is done elsewhere.
        // NOTE: Please add all backends before calling maintainRequestHandlers.
        void addBackend(Backend& local_backend, bool blocking_mode, Request request);
        Http3ClientBackend& getBackend(const std::string& url);

        // There can be multiple request streams per backend, so each new request
        // needs to get a new stream identifier from the quic connector, and then
        // establish the response handler which will be a wrapper of the
        // HTTP3ClientBackend processRequestStream.
        // This also will periodically solicit a journal request for the server (from each backend).
        void maintainRequestHandlers(Communication& connector, double time);

        // The HTTP3ClientBackendUpdater does not need a processRequestStream method,
        // because the greater thread will call the Quic Connector processRequestStream method,
        // which will indirectly call the HTTP3ClientBackend processRequestStream method.
        // in the established response handler.

    private:
        // Track the backends which will be updated by this class.  
        std::vector<Http3ClientBackend> backends_;

        std::map<StreamIdentifier, HTTP3TreeMessage> ongoingRequests_;
        std::map<StreamIdentifier, Http3ClientBackend&> journalingRequests_;
        size_t journalRequestsPerMinute_;
        double lastTime_ = 0.0;

        std::list<StreamIdentifier> completeRequests_;
};