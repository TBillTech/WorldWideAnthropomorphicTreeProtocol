#pragma once

#include "backend.h"
#include "communication.h"

// The journaling feature interfaces with the http3 server to request the latest list of notifications beyond the client's current.
// Moreover, since the server does not push notifications, it will replay notifications up to some past limit whenever the client desires.

class Http3ClientBackend : public Backend {
public:
    Http3ClientBackend(Backend& local_backend, bool blocking_mode = true)
        : localBackend_(local_backend), lastNotification_({0, {}}), blockingMode_(blocking_mode) {};
    ~Http3ClientBackend() override = default;

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

    void registerNodeListener(const std::string listener_name, const std::string label_rule, NodeListenerCallback callback) override;
    void deregisterNodeListener(const std::string listener_name, const std::string label_rule) override;

    // Notify listeners for a specific label rule.
    void notifyListeners(const std::string& label_rule, const fplus::maybe<TreeNode>& node) override;

    void processNotification() override {};

    chunks processRequestStream(const StreamIdentifier& stream_id, chunks& request); 

private:
    Backend& localBackend_;
    // The client tracks the last notification in order to keep up with changes on the server.
    // id == 0, empty means client need to completely reload the mutable part of the tree (at least).
    // The client does NOT need to keep the full journal of notifications
    using Notification = std::pair<std::string, fplus::maybe<TreeNode>>;
    using SequentialNotification = std::pair<uint64_t, Notification>;
    SequentialNotification lastNotification_;
    std::vector<SequentialNotification> pendingNotifications_;

    // Mutex to lock the handler requests
    mutable std::mutex handlerMutex_;
    // Transaction is how the changes are tracked and sent to the server.
    Transaction transaction_;
    bool blockingMode_;
};

// The Client Backend Updater sets up a StreamIdentifier for each Http3ClientBackend registered with it.
// When processing the RequestStream, the handler registered with the Quic_Connector will call the Http3ClientBackend::chunkStreamHandler,
// which will then lock the Http3ClientBackend and process the incoming information.
// Meanwhile, the higher level client program will simply use the backends (and be forced to wait on the mutexes if they are busy). 
class Http3ClientBackendUpdater {
    public:
        Http3ClientBackendUpdater() = default;
        ~Http3ClientBackendUpdater() = default;

        // Add a backend to the updater. This method should only be used with Request object that are tree urls.
        // Getting static assets at Request urls is done elsewhere.
        void addBackend(Request req, Http3ClientBackend& backend);

        // Process the request stream, which will be called by the Quic_Connector's handler for the request stream.
        chunks processRequestStream(const StreamIdentifier& stream_id, chunks& request); 

    private:
        // The backend is a map of stream identifiers to backends.
        std::map<StreamIdentifier, Http3ClientBackend&> backends_;
        // The backend is a map of stream identifiers to request objects.
        std::map<StreamIdentifier, Request> requests_;
};