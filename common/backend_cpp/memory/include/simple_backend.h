#pragma once

#include "backend.h"
#include "memory_tree.h"
#include <map>

bool checkLabelRuleOverlap(const std::string& label_rule_1, const std::string& label_rule_2);

// A simple in-memory implementation of the Backend interface.
class SimpleBackend : public Backend {
public:
    SimpleBackend(MemoryTree & memory_tree) : memory_tree_(memory_tree) {};
    ~SimpleBackend() override = default;

    // Retrieve a node by its label rule.
    std::optional<TreeNode> getNode(const std::string& label_rule) const override;

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

    // Notify listeners for a specific label rule.
    void notifyListeners(const std::string& label_rule, const std::optional<TreeNode>& node);

private:
    MemoryTree &memory_tree_;
    std::map<std::string, std::pair<NodeListenerCallback, std::vector<std::string>>> node_listeners_;
};
