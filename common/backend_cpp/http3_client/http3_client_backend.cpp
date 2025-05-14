#include "http3_client_backend.h"
#include "http3_tree_message.h"

void Http3ClientBackend::awaitResponse()
{
    std::unique_lock<std::mutex> lock(blockingMutex_);
    responseReady = false; // Reset before waiting for the next response
    responseCondition.wait(lock, [&]{ return responseReady; }); // blocks until ready is true
}

bool Http3ClientBackend::awaitBoolResponse()
{
    awaitResponse();
    std::lock_guard<std::mutex> lock(handlerMutex_);
    bool result = boolResponse;
    boolResponse = false; // Reset for next response
    return result;
}

fplus::maybe<TreeNode> Http3ClientBackend::awaitTreeResponse()
{
    awaitResponse();
    std::lock_guard<std::mutex> lock(handlerMutex_);
    fplus::maybe<TreeNode> result = maybeTreeResponse_;
    maybeTreeResponse_ = fplus::nothing<TreeNode>(); // Reset for next response
    return result;
}

std::vector<TreeNode> Http3ClientBackend::awaitVectorTreeResponse()
{
    awaitResponse();
    std::lock_guard<std::mutex> lock(handlerMutex_);
    std::vector<TreeNode> result = vectorTreeResponse_;
    vectorTreeResponse_.clear(); // Reset for next response
    return result;
}

void Http3ClientBackend::responseSignal()
{
    std::lock_guard<std::mutex> lock(blockingMutex_);
    responseReady = true;
    responseCondition.notify_one();
}

fplus::maybe<TreeNode> Http3ClientBackend::getNode(const std::string& label_rule) const {
    // Use the caching localBackend_ to get the node
    std::lock_guard<std::mutex> lock(backendMutex_);
    return localBackend_.getNode(label_rule);
}

bool Http3ClientBackend::upsertNode(const std::vector<TreeNode>& nodes) {
    // Use the caching localBackend_ to upsert the node
    bool result(false);
    {
        std::lock_guard<std::mutex> lock(backendMutex_);
        result = localBackend_.upsertNode(nodes);
    }
    // But upsertNode also needs to send to the server.
    {
        std::lock_guard<std::mutex> lock(handlerMutex_);
        if (!serverSyncOn_)
        {
            return result;
        }
        HTTP3TreeMessage request(0);
        request.encode_upsertNodeRequest(nodes);
        pendingRequests_.push_back(std::move(request));
    }
    if (!blockingMode_) {
        return result;
    }
    return awaitBoolResponse();
}

bool Http3ClientBackend::deleteNode(const std::string& label_rule) {
    // Use the caching localBackend_ to delete the node
    bool result(false);
    {
        std::lock_guard<std::mutex> lock(backendMutex_);
        result = localBackend_.deleteNode(label_rule);
    }
    // But deleteNode also needs to send to the server.
    {
        std::lock_guard<std::mutex> lock(handlerMutex_);
        if (!serverSyncOn_)
        {
            return result;
        }
        HTTP3TreeMessage request(0);
        request.encode_deleteNodeRequest(label_rule);
        pendingRequests_.push_back(std::move(request));
    }
    if (!blockingMode_) {
        return result;
    }
    return awaitBoolResponse();
}

std::vector<TreeNode> Http3ClientBackend::getPageTree(const std::string& page_node_label_rule) const {
    // Use the caching localBackend_ to get the page tree
    std::lock_guard<std::mutex> lock(backendMutex_);
    return localBackend_.getPageTree(page_node_label_rule);
}

std::vector<TreeNode> Http3ClientBackend::relativeGetPageTree(const TreeNode& node, const std::string& page_node_label_rule) const {
    // create the relative_page_label_rule
    std::string relative_page_label_rule = node.getLabelRule() + "/" + page_node_label_rule;
    return getPageTree(relative_page_label_rule);
}

std::vector<TreeNode> Http3ClientBackend::queryNodes(const std::string& label_rule) const {
    // Use the caching localBackend_ to query the nodes
    std::lock_guard<std::mutex> lock(backendMutex_);
    return localBackend_.queryNodes(label_rule);
}

std::vector<TreeNode> Http3ClientBackend::relativeQueryNodes(const TreeNode& node, const std::string& label_rule) const {
    // create the relative_label_rule
    std::string relative_label_rule = node.getLabelRule() + "/" + label_rule;
    return queryNodes(relative_label_rule);
}

bool Http3ClientBackend::openTransactionLayer(const TreeNode& node) {
    // Use the caching localBackend_ to open the transaction layer
    bool result(false);
    {
        std::lock_guard<std::mutex> lock(backendMutex_);
        result = localBackend_.openTransactionLayer(node);
        if(!result) {
            return false;
        }
    }
    // But openTransactionLayer also needs to send to the server.
    {
        std::lock_guard<std::mutex> lock(handlerMutex_);
        if (!serverSyncOn_)
        {
            return result;
        }
        HTTP3TreeMessage request(0);
        request.encode_openTransactionLayerRequest(node);
        pendingRequests_.push_back(std::move(request));
    }
    if (!blockingMode_) {
        return result;
    }
    result = awaitBoolResponse();
    if (!result) {
        std::lock_guard<std::mutex> lock(backendMutex_);
        localBackend_.closeTransactionLayers();
    }
    return result;
}

bool Http3ClientBackend::closeTransactionLayers(void) {
    // Use the caching localBackend_ to close the transaction layers
    bool result(false);
    {
        std::lock_guard<std::mutex> lock(backendMutex_);
        result = localBackend_.closeTransactionLayers();
    }
    // But closeTransactionLayers also needs to send to the server.
    {
        std::lock_guard<std::mutex> lock(handlerMutex_);
        if (!serverSyncOn_)
        {
            return result;
        }
        HTTP3TreeMessage request(0);
        request.encode_closeTransactionLayersRequest();
        pendingRequests_.push_back(std::move(request));
    }
    if (!blockingMode_) {
        return result;
    }
    return awaitBoolResponse();
}

bool Http3ClientBackend::applyTransaction(const Transaction& transaction) {
    // ApplyTransaction _surely_ needs to send to the server.
    {
        std::lock_guard<std::mutex> lock(handlerMutex_);
        if (serverSyncOn_)
        {
            HTTP3TreeMessage request(0);
            request.encode_applyTransactionRequest(transaction);
            pendingRequests_.push_back(std::move(request));
        }
    }
    if (!blockingMode_ || !serverSyncOn_) {
        std::lock_guard<std::mutex> lock(backendMutex_);
        return localBackend_.applyTransaction(transaction);
    } 
    if (awaitBoolResponse()) {
        std::lock_guard<std::mutex> lock(backendMutex_);
        return localBackend_.applyTransaction(transaction);
    }
    return false;
}

std::vector<TreeNode> Http3ClientBackend::getFullTree() const {
    // Use the caching localBackend_ to get the full tree
    std::lock_guard<std::mutex> lock(backendMutex_);
    return localBackend_.getFullTree();
}

void Http3ClientBackend::registerNodeListener(const std::string listener_name, const std::string label_rule, bool child_notify, NodeListenerCallback callback) {
    // Since the user of the http3 client backend is asking to listen for changes, 
    // in fact, the listeners need to be registered both with the local backend and the server.
    // In cases where the local backend sees a change, then the server will eventually duplicate the notification.
    // Therefore, we need to track local notifications and deduplicate them versus server notifications.
    std::lock_guard<std::mutex> lock(backendMutex_);
    NodeListenerCallback local_callback = [this, callback](Backend& backend, const std::string label_rule, const fplus::maybe<TreeNode> node) {
        // Notify the local backend
        callback(*this, label_rule, node);
    };
    localBackend_.registerNodeListener(listener_name, label_rule, child_notify, local_callback);
    // Register the listener with the server
    {
        std::lock_guard<std::mutex> lock(handlerMutex_);
        if (serverSyncOn_)
        {
            HTTP3TreeMessage request(0);
            request.encode_registerNodeListenerRequest(listener_name, label_rule, child_notify);
            pendingRequests_.push_back(std::move(request));
        }
    }
    // But registerNodeListner is not the kind of method that blocks, so done here.
}

void Http3ClientBackend::deregisterNodeListener(const std::string listener_name, const std::string label_rule) {
    std::lock_guard<std::mutex> lock(backendMutex_);
    localBackend_.deregisterNodeListener(listener_name, label_rule);
    // Deregister the listener with the server
    {
        std::lock_guard<std::mutex> lock(handlerMutex_);
        if (serverSyncOn_) {
            HTTP3TreeMessage request(0);
            request.encode_deregisterNodeListenerRequest(listener_name, label_rule);
            pendingRequests_.push_back(std::move(request));
        }
    }
}

void Http3ClientBackend::notifyListeners(const std::string& label_rule, const fplus::maybe<TreeNode>& node) {
    // Notify the local backend
    std::lock_guard<std::mutex> lock(backendMutex_);
    localBackend_.notifyListeners(label_rule, node);
    // Notify the server
    {
        std::lock_guard<std::mutex> lock(handlerMutex_);
        if (serverSyncOn_) {
            HTTP3TreeMessage request(0);
            request.encode_notifyListenersRequest(label_rule, node);
            pendingRequests_.push_back(std::move(request));
        }
    }
}

void Http3ClientBackend::processHTTP3Response(HTTP3TreeMessage& response) {
    // Process the response based on the signal
    switch (response.getSignal()) {
        case payload_chunk_header::SIGNAL_WWATP_GET_NODE_RESPONSE: {
                fplus::maybe<TreeNode> node = response.decode_getNodeResponse();
                {
                    std::lock_guard<std::mutex> lock(handlerMutex_);
                    maybeTreeResponse_ = node;
                }
                if (blockingMode_) {
                    responseSignal();
                } else {
                    disableServerSync();
                    if (node.is_just()) {
                        upsertNode({node.unsafe_get_just()});
                    } else {
                        deleteNode(response.decode_deleteNodeRequest());
                    }
                    enableServerSync();
                }
            }
            break;
        case payload_chunk_header::SIGNAL_WWATP_UPSERT_NODE_RESPONSE: {
                bool success = response.decode_upsertNodeResponse();
                {
                    std::lock_guard<std::mutex> lock(handlerMutex_);
                    boolResponse = success;
                }
                if (blockingMode_) {
                    responseSignal();
                }
            }
            break;
        case payload_chunk_header::SIGNAL_WWATP_DELETE_NODE_RESPONSE: {
                bool success = response.decode_deleteNodeResponse();
                {
                    std::lock_guard<std::mutex> lock(handlerMutex_);
                    boolResponse = success;
                }
                if (blockingMode_) {
                    responseSignal();
                } 
            }
            break;
        case payload_chunk_header::SIGNAL_WWATP_GET_PAGE_TREE_RESPONSE: {
                std::vector<TreeNode> nodes = response.decode_getPageTreeResponse();
                // Alternatively, if the request page was the mutable page tree, this should never do the
                // response signal.
                string mutable_page_tree_label_rule = "MutableNodes";
                {
                    std::lock_guard<std::mutex> lock(handlerMutex_);
                    mutable_page_tree_label_rule = mutablePageTreeLabelRule_;
                }
                if (response.decode_getPageTreeRequest() == mutablePageTreeLabelRule_) {
                    disableServerSync();
                    upsertNode(nodes);
                    enableServerSync();
                } else {
                    {
                        std::lock_guard<std::mutex> lock(handlerMutex_);
                        vectorTreeResponse_ = nodes;
                    }
                    if (blockingMode_) {
                        responseSignal();
                    } else {
                        disableServerSync();
                        upsertNode(nodes);
                        enableServerSync();
                    }
                }
            }
            break;
        case payload_chunk_header::SIGNAL_WWATP_QUERY_NODES_RESPONSE: {
                std::vector<TreeNode> nodes = response.decode_getQueryNodesResponse();
                {
                    std::lock_guard<std::mutex> lock(handlerMutex_);
                    vectorTreeResponse_ = nodes;
                }
                if (blockingMode_) {
                    responseSignal();
                } else {
                    disableServerSync();
                    upsertNode(nodes);
                    enableServerSync();
                }
            }
            break;
        case payload_chunk_header::SIGNAL_WWATP_OPEN_TRANSACTION_LAYER_RESPONSE: {
                bool success = response.decode_openTransactionLayerResponse();
                {
                    std::lock_guard<std::mutex> lock(handlerMutex_);
                    boolResponse = success;
                }
                if (blockingMode_) {
                    responseSignal();
                }
            }
            break;
        case payload_chunk_header::SIGNAL_WWATP_CLOSE_TRANSACTION_LAYERS_RESPONSE: {
                bool success = response.decode_closeTransactionLayersResponse();
                {
                    std::lock_guard<std::mutex> lock(handlerMutex_);
                    boolResponse = success;
                }
                if (blockingMode_) {
                    responseSignal();
                }
            }
            break;
        case payload_chunk_header::SIGNAL_WWATP_APPLY_TRANSACTION_RESPONSE: {
                bool success = response.decode_applyTransactionResponse();
                {
                    std::lock_guard<std::mutex> lock(handlerMutex_);
                    boolResponse = success;
                }
                if (blockingMode_) {
                    responseSignal();
                }
            }
            break;
        case payload_chunk_header::SIGNAL_WWATP_GET_FULL_TREE_RESPONSE: {
                std::vector<TreeNode> nodes = response.decode_getFullTreeResponse();
                {
                    std::lock_guard<std::mutex> lock(handlerMutex_);
                    vectorTreeResponse_ = nodes;
                }
                if (blockingMode_) {
                    responseSignal();
                } else {
                    disableServerSync();
                    deleteNode("");
                    upsertNode(nodes);
                    enableServerSync();
                }
            }
            break;
        case payload_chunk_header::SIGNAL_WWATP_REGISTER_LISTENER_RESPONSE: {
            }
            break;
        case payload_chunk_header::SIGNAL_WWATP_DEREGISTER_LISTENER_RESPONSE: {
            }
            break;
        case payload_chunk_header::SIGNAL_WWATP_NOTIFY_LISTENERS_RESPONSE: {
            }
            break;
        case payload_chunk_header::SIGNAL_WWATP_PROCESS_NOTIFICATION_RESPONSE: {
            }
            break;
        case payload_chunk_header::SIGNAL_WWATP_GET_JOURNAL_RESPONSE: {
                std::vector<SequentialNotification> notifications = response.decode_getJournalResponse();
                if ((notifications.front().first == 0) && (notifications.back().first > notifications.size() - 1)) {
                    getMutablePageTree();
                } else {
                    for (const auto& notification : notifications) {
                        processNotification(notification);
                    }
                }
                updateLastNotification(notifications.back());
            }
            break;
        default:
            // Handle unknown signal
            std::cerr << "Unknown signal: " << static_cast<int>(response.getSignal()) << std::endl;
            break;
        }
}

fplus::maybe<HTTP3TreeMessage> Http3ClientBackend::popNextRequest() {
    std::lock_guard<std::mutex> lock(handlerMutex_);
    if (pendingRequests_.empty()) {
        return fplus::nothing<HTTP3TreeMessage>();
    }
    HTTP3TreeMessage request(pendingRequests_.front());
    pendingRequests_.pop_front();
    return request;
}

HTTP3TreeMessage Http3ClientBackend::solicitJournalRequest() const {
    HTTP3TreeMessage request(0);
    std::lock_guard<std::mutex> lock(handlerMutex_);
    request.encode_getJournalRequest(lastNotification_);
    return request;
}

void Http3ClientBackend::requestFullTreeSync() {
    // requestFullTreeSync explicitly from the server.
    {
        HTTP3TreeMessage request(0);
        request.encode_getFullTreeRequest();
        std::lock_guard<std::mutex> lock(handlerMutex_);
        pendingRequests_.push_back(std::move(request));
    }
    if(!blockingMode_) {
        return;
    }
    // Wait for the response
    auto nodes = awaitVectorTreeResponse();
    disableServerSync();
    deleteNode("");
    upsertNode(nodes);
    enableServerSync();
    std::lock_guard<std::mutex> lock(backendMutex_);
}

void Http3ClientBackend::getMutablePageTree() {
    // Use the caching localBackend_ to get the mutable page tree
    string label_rule = "MutableNodes";
    {
        std::lock_guard<std::mutex> lock(handlerMutex_);
        auto label_rule = mutablePageTreeLabelRule_;
    }
    {
        HTTP3TreeMessage request(0);
        request.encode_getPageTreeRequest(label_rule);
        std::lock_guard<std::mutex> lock(handlerMutex_);
        pendingRequests_.push_back(std::move(request));
    }
    if(!blockingMode_) {
        return;
    }
    // Wait for the response
    auto nodes = awaitVectorTreeResponse();
    disableServerSync();
    upsertNode(nodes);
    enableServerSync();
    std::lock_guard<std::mutex> lock(backendMutex_);
    needMutablePageTreeSync_ = false;
}

void Http3ClientBackend::processNotification(SequentialNotification const& notification) {
    uint64_t sequence_num = notification.first;
    Notification const& notification_data = notification.second;
    std::string label_rule = notification_data.first;
    fplus::maybe<TreeNode> const& node = notification_data.second;
    if (node.is_just()) {
        disableServerSync();
        upsertNode({node.unsafe_get_just()});
        enableServerSync();
    } else {
        disableServerSync();
        deleteNode(label_rule);
        enableServerSync();
    }
}

void Http3ClientBackend::updateLastNotification(SequentialNotification const& notification) {
    std::lock_guard<std::mutex> lock(backendMutex_);
    lastNotification_ = notification;
}
