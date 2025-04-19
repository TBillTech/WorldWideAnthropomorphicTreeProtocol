#pragma once

#include "backend.h"
#include <mutex>

// The ThreadsafeBackend class is a layer on top of another Backend that provides thread safe access.
// For most of the methods, it simply locks the mutex, and delegates to the underlying Backend.
// However, node listeners are a little more complicated.
// The ThreadsafeBackend class will also track node nofications in a stack, and provide a processNotification 
// method for worker threads to deal with the notifications asynchronously without blocking other threads from 
// accessing the tree.
class ThreadsafeBackend : public Backend {
public:
    ThreadsafeBackend(Backend & tree) : tree_(tree) {};
    ~ThreadsafeBackend() override = default;

    // Retrieve a node by its label rule.
    fplus::maybe<TreeNode> getNode(const std::string& label_rule) const override;

    // Add or update a parent node and its children in the tree.
    bool upsertNode(const std::vector<TreeNode>& nodes) override;

    // Delete a node and its children from the tree.
    bool deleteNode(const std::string& label_rule) override;

    // Query nodes matching a label rule.
    std::vector<TreeNode> queryNodes(const std::string& label_rule) const override;

    bool applyTransaction(const Transaction& transaction) override;

    // Retrieve the entire tree structure (for debugging or full sync purposes).
    std::vector<TreeNode> getFullTree() const override;

    void registerNodeListener(const std::string listener_name, const std::string label_rule, NodeListenerCallback callback) override;
    void deregisterNodeListener(const std::string listener_name, const std::string label_rule) override;

    void notifyListeners(const std::string& label_rule, const fplus::maybe<TreeNode>& node);

    // Process one notification for a specific label rule.  
    void processNotification();

private:
    Backend& tree_;
    mutable std::mutex tree_mutex_;
    mutable std::mutex notification_mutex_;
    std::vector<std::pair<NodeListenerCallbackArgs, NodeListenerCallback>> notification_stack_;
};