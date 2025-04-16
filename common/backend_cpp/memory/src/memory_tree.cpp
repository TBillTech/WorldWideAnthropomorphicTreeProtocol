#include "memory_tree.h"
#include <algorithm>
#include <stdexcept>

std::optional<TreeNode> MemoryTree::getNode(const std::string& label_rule) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return unsafeGetNode(label_rule);
}

std::optional<TreeNode> MemoryTree::unsafeGetNode(const std::string& label_rule) const {
    auto it = tree_.find(label_rule);
    if (it != tree_.end()) {
        return it->second;
    }
    return std::nullopt;
}

bool MemoryTree::upsertNode(const std::vector<TreeNode>& nodes) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& node : nodes) {
        tree_[node.label_rule] = node;
    }
    return true;
}

bool MemoryTree::deleteNode(const std::string& label_rule) {
    std::lock_guard<std::mutex> lock(mutex_);
    return unsafeDeleteNode(label_rule);
}

bool MemoryTree::unsafeDeleteNode(const std::string& label_rule) {
    auto node = unsafeGetNode(label_rule);
    if (!node) {
        return false;
    }

    // Recursively delete children
    std::vector<std::string> to_delete = {label_rule};
    while (!to_delete.empty()) {
        auto current = to_delete.back();
        to_delete.pop_back();

        auto current_node = unsafeGetNode(current);
        if (current_node) {
            const auto& children = current_node->child_names;
            to_delete.insert(to_delete.end(), children.begin(), children.end());
            tree_.erase(current);
        }
    }

    return true;
}

std::vector<TreeNode> MemoryTree::queryNodes(const std::string& label_rule) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TreeNode> result;
    for (const auto& [key, node] : tree_) {
        if (key.find(label_rule) != std::string::npos) {
            result.push_back(node);
        }
    }
    return result;
}

std::vector<TreeNode> MemoryTree::getFullTree() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TreeNode> result;
    for (const auto& [key, node] : tree_) {
        result.push_back(node);
    }
    return result;
}

// Try to apply a subtransaction to the tree.
// This will throw an exception if canPerformSubTransaction() fails.
bool MemoryTree::applyTransaction(const Transaction& transaction)
{
    std::lock_guard<std::mutex> lock(mutex_);
    // First check all the sub transactions to verify them.
    for (const auto& sub_transaction : transaction) {
        if (!canPerformSubTransaction(sub_transaction)) {
            // runtime error includes the label rule of the node that failed, and the number of children changes
            throw std::runtime_error("Transaction is not valid: " + sub_transaction.first.second.first + " with " +
                std::to_string(sub_transaction.second.size()) + " modified children.");
        }
    }
    // Then perform the sub transactions
    for (const auto& sub_transaction : transaction) {
        if (!performSubTransaction(sub_transaction)) {
            // runtime error includes the label rule of the node that failed, and the number of children changes
            throw std::runtime_error("Transaction failed to apply: " + sub_transaction.first.second.first + " with " +
                std::to_string(sub_transaction.second.size()) + " modified children.");
        }
    }
    return true;
}

// Check if a SubTransaction can be performed without error.
bool MemoryTree::canPerformSubTransaction(const SubTransaction& sub_transaction) const
{
    // Check if the parent node exists
    const auto& parent_node_label = sub_transaction.first.second.first;
    auto it = tree_.find(parent_node_label);
    if (it == tree_.end()) {
        if (!sub_transaction.first.first.has_value()) {
            return true; // The transaction expected there was no prior node at all, and it is missing, so good.
        }
        return false; // Parent node does not exist, but was expected by the transaction
    }
    // Then check if the parent node sequence number matches the old version 
    if (it->second.version.version_number != sub_transaction.first.first.value()) {
        return false; // Parent node version does not match
    }
    // Check that the new version number is 1 greater than the old version number, (or wraps to 0 if at max)
    uint16_t next_version = (it->second.version.version_number + 1) % (it->second.version.max_version_sequence);
    if (sub_transaction.first.second.second.has_value()) {
        if (next_version != sub_transaction.first.second.second.value().version.version_number) {
            return false; // New version number is not an increment
        }
    }

    // Check each of the child nodes to make sure some other modification has not occurred
    for (const auto& child : sub_transaction.second) {
        auto old_child_it = tree_.find(child.second.first);
        if (old_child_it != tree_.end()) {
            if (!child.first.has_value()) { // No version supplied so
                // The attempted transaction was expecting no child in the tree
                // This is OK if and only if the child is being deleted
                if (child.second.second.has_value()) {
                    return false; // Child node is not being deleted, but the transaction was expecting it to exist
                }
            } else if (old_child_it->second.version.version_number != child.first.value()) {
                return false; // Child node version does not match that expected by the transaction
            }    
        } else {
            if (child.first.has_value()) {
                return false; // Child node does not exist, but the transaction was expecting it to
            }
        }
    }

    return true;
}

// ASSUMING THAT THE TRANSACTION IS VALID, perform the transaction.
bool MemoryTree::performSubTransaction(const SubTransaction& sub_transaction)
{
    // Update the parent node
    auto& parent_node = sub_transaction.first.second.second;
    if (parent_node.has_value()) {
        tree_[parent_node.value().label_rule] = parent_node.value();
    } else {
        tree_.erase(sub_transaction.first.second.first); // Delete the parent node
    }

    // Update or delete child nodes
    for (const auto& child : sub_transaction.second) {
        const auto& child_node = child.second;
        if (child.second.second.has_value()) {
            tree_[child.second.first] = child.second.second.value(); // Update existing child node
        } else {
            tree_.erase(child.second.first); // Delete child node
        }
    }

    return true;
}
