#pragma once

#include "backend.h"
#include "memory_tree.h"
#include <mutex>

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

    bool openTransactionLayer(const TreeNode& node) override;

    bool closeTransactionLayers(void) override;

    // Retrieve the entire tree structure (for debugging or full sync purposes).
    std::vector<TreeNode> getFullTree() const override;

private:
    MemoryTree &memory_tree_;
    Transaction transaction_stack_;
};
