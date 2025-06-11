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
                       Request request, size_t journalRequestsPerMinute = 60,
                       fplus::maybe<TreeNode> staticNode = fplus::maybe<TreeNode>())
        : localBackend_(local_backend), lastNotification_({0, {}}), 
          blockingMode_(blocking_mode), requestUrlInfo_(request),
          journalRequestsPerMinute_(journalRequestsPerMinute),
          staticNode_(staticNode) {
            if ((staticNode_.is_just()) && request.isWWATP()) {
                throw std::runtime_error("Static node must be used with static request (not WWATP).");
            }
          };
    ~Http3ClientBackend() override = default;

    Http3ClientBackend(Http3ClientBackend&& other) noexcept :
        localBackend_(other.localBackend_),
        needMutablePageTreeSync_(other.needMutablePageTreeSync_),
        handlerMutex_(),
        lastNotification_(std::move(other.lastNotification_)),
        mutablePageTreeLabelRule_(std::move(other.mutablePageTreeLabelRule_)),
        serverSyncOn_(other.serverSyncOn_),
        blockingMode_(other.blockingMode_),
        blockingMutex_(),
        responseCondition(),
        responseReady(other.responseReady),
        boolResponse(other.boolResponse),
        maybeTreeResponse_(std::move(other.maybeTreeResponse_)),
        vectorTreeResponse_(std::move(other.vectorTreeResponse_)),
        chunksResponse(std::move(other.chunksResponse)),
        requestUrlInfo_(std::move(other.requestUrlInfo_)),
        pendingRequests_(std::move(other.pendingRequests_)),
        journalRequestsPerMinute_(other.journalRequestsPerMinute_),
        lastJournalRequestTime_(other.lastJournalRequestTime_),
        staticNode_(std::move(other.staticNode_)),
        notificationBlock_(other.notificationBlock_.load())
    {
    }
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

    void processNotification() override;

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
    void setMutablePageTreeLabelRule(std::string label_rule = "MutableNodes");
    void getMutablePageTree();

    bool haveJournalRequest(double time) {
        if ((journalRequestsPerMinute_ == 0) || (lastJournalRequestTime_ <= 1.0)) {
            lastJournalRequestTime_ = time;
            return false;
        }
        double timeDelta = time - lastJournalRequestTime_;
        lastJournalRequestTime_ = time;
        return (timeDelta*journalRequestsPerMinute_ > 60.0);
    }

private:
    void requestStaticNodeData();
    void setNodeChunks(chunks& chunks);

    void getMutablePageTreeInMode(bool blocking_mode);

    void awaitResponse();
    bool awaitBoolResponse();
    fplus::maybe<TreeNode> awaitTreeResponse();
    std::vector<TreeNode> awaitVectorTreeResponse();
    chunks awaitChunksResponse();

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
    chunks chunksResponse;

    const Request requestUrlInfo_;
    
    std::list<HTTP3TreeMessage> pendingRequests_;

    size_t journalRequestsPerMinute_;
    double lastJournalRequestTime_ = 0.0;

    fplus::maybe<TreeNode> staticNode_;
    atomic<bool> notificationBlock_{false};  // Used to block until a journal notification has been processed
};

class Communication;

class Http3ClientBackendUpdater {
    public:
        Http3ClientBackendUpdater() = default;
        ~Http3ClientBackendUpdater() = default;

        // Delete copy constructor and copy assignment operator
        Http3ClientBackendUpdater(const Http3ClientBackendUpdater& other) = delete;
        Http3ClientBackendUpdater& operator=(const Http3ClientBackendUpdater&) = delete;

        // Add a backend to the updater. This method should only be used with Request object that are tree urls.
        // Getting static assets at Request urls is done elsewhere.
        // NOTE: Please add all backends before calling maintainRequestHandlers.
        Http3ClientBackend& addBackend(Backend& local_backend, bool blocking_mode, Request request);
        Http3ClientBackend& getBackend(const std::string& url);

        // There can be multiple request streams per backend, so each new request
        // needs to get a new stream identifier from the quic connector, and then
        // establish the response handler which will be a wrapper of the
        // HTTP3ClientBackend processRequestStream.
        // This also will periodically solicit a journal request for the server (from each backend).
        void maintainRequestHandlers(Communication& connector, double time);

        // It is expected that the main thread will use this class in one of two modes:
        // 1. In an explicit work mode, which feathers maintaining the handlers with
        //    the processRequestStream method in some fashion, such as:
        //    while (true) {
        //        updater.maintainRequestHandlers(connector, time);
        //        connector.processRequestStream();
        //        sleep_for(std::chrono::milliseconds(100));
        //        time += 0.1;  // track the time for journaling purposes
        //    }
        // 2. As a daemon thread, using the below function, which will create new thread 
        //    and do the above work until the stop flag is set to true.
        // NOTE: Using the block mode of the Http3ClientBackend is most easily done in mode 2.
        void run(Communication& connector, double time, size_t sleep_milli = 100) {
            stopFlag.store(false);
            updaterThread_ = thread([this, &connector, &time, sleep_milli]() {
                while (!stopFlag.load()) {
                    maintainRequestHandlers(connector, time);
                    connector.processRequestStream();
                    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_milli));
                    time += 0.1;  // track the time for journaling purposes
                }
                return EXIT_SUCCESS;
            });
        }
        void stop() {
            stopFlag.store(true);
        }

    private:
        // Track the backends which will be updated by this class.  
        std::list<Http3ClientBackend> backends_;

        std::map<StreamIdentifier, HTTP3TreeMessage> ongoingRequests_;
        std::map<StreamIdentifier, Http3ClientBackend&> journalingRequests_;
        double lastTime_ = 0.0;

        std::list<StreamIdentifier> completeRequests_;

        thread updaterThread_;  // Thread to run the updater
        atomic<bool> stopFlag{false};  // Flag to stop the updater thread
};