#pragma once

#include "backend.h"
#include <map>

// A simple in-memory implementation of the Backend interface.
class TransactionalBackend : public Backend {
public:
    TransactionalBackend(Backend & tree) : tree_(tree) {};
    ~TransactionalBackend() override = default;

    // Retrieve a node by its label rule.
    std::optional<TreeNode> getNode(const std::string& label_rule) const override;

    // Add or update a parent node and its children in the tree.
    bool upsertNode(const std::vector<TreeNode>& nodes) override;

    // Delete a node and its children from the tree.
    bool deleteNode(const std::string& label_rule) override;

    // Query nodes matching a label rule.
    std::vector<TreeNode> queryNodes(const std::string& label_rule) const override;

    bool openTransactionLayer(const TreeNode& node);

    bool closeTransactionLayers(void);

    bool applyTransaction(const Transaction& transaction) override;

    // Retrieve the entire tree structure (for debugging or full sync purposes).
    std::vector<TreeNode> getFullTree() const override;

    void registerNodeListener(const std::string listener_name, const std::string label_rule, NodeListenerCallback callback) override;
    void deregisterNodeListener(const std::string listener_name, const std::string label_rule) override;

    void notifyListeners(const std::string& label_rule, const std::optional<TreeNode>& node);

private:
    Backend& tree_;
    Transaction transaction_stack_;
};
