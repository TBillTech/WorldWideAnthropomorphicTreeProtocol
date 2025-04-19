#include "memory_tree.h"
#include <algorithm>
#include <stdexcept>

using namespace fplus;

maybe<TreeNode> MemoryTree::getNode(const std::string& label_rule) const {
    auto it = tree_.find(label_rule);
    if (it != tree_.end()) {
        return it->second;
    }
    return maybe<TreeNode>();
}

bool MemoryTree::upsertNode(const std::vector<TreeNode>& nodes) {
    for (const auto& node : nodes) {
        tree_[node.getLabelRule()] = node;
    }
    return true;
}

bool MemoryTree::deleteNode(const std::string& label_rule) {
    auto findnode = tree_.find(label_rule);
    if (findnode == tree_.end()) {
        return false;
    }
    std::vector<std::string> to_delete = findnode->second.getChildNames();
    tree_.erase(findnode);

    // Recursively delete children
    while (!to_delete.empty()) {
        auto current = to_delete.back();
        to_delete.pop_back();

        auto current_node = tree_.find(current);
        if (current_node != tree_.end()) {
            auto children = current_node->second.getChildNames();
            to_delete.insert(to_delete.end(), children.begin(), children.end());
            tree_.erase(current);
        }
    }

    return true;
}

std::vector<TreeNode> MemoryTree::queryNodes(const std::string& label_rule) const {
    std::vector<TreeNode> result;
    for (const auto& [key, node] : tree_) {
        if (key.find(label_rule) != std::string::npos) {
            result.push_back(node);
        }
    }
    return result;
}

std::vector<TreeNode> MemoryTree::getFullTree() const {
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
        if (sub_transaction.first.first.is_nothing()) {
            return true; // The transaction expected there was no prior node at all, and it is missing, so good.
        }
        return false; // Parent node does not exist, but was expected by the transaction
    }
    // Then check if the parent node sequence number matches the old version 
    if (maybe(it->second.getVersion().version_number) != sub_transaction.first.first) {
        return false; // Parent node version does not match
    }
    // Check that the new version number is 1 greater than the old version number, (or wraps to 0 if at max)
    maybe<uint16_t> next_version((it->second.getVersion().version_number + 1) % (it->second.getVersion().max_version_sequence));
    if (next_version != sub_transaction.first.second.second.and_then([](const auto& node) { return maybe(node.getVersion().version_number); })) {
        return false; // New version number is not an increment
    }

    // Check each of the child nodes to make sure some other modification has not occurred
    for (auto child : sub_transaction.second) {
        auto old_child_it = tree_.find(child.second.first);
        if (old_child_it != tree_.end()) {
            if (child.first.is_nothing()) { // No version supplied so
                // The attempted transaction was expecting no child in the tree
                // This is OK if and only if the child is being deleted
                if (child.second.second.is_just()) {
                    return false; // Child node is not being deleted, but the transaction was expecting it to exist
                }
            } else if (maybe(old_child_it->second.getVersion().version_number) != child.first) {
                return false; // Child node version does not match that expected by the transaction
            }    
        } else { // old_child_it == tree_.end()
            if (child.first.is_just()) {
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
    if (parent_node.is_just()) {
        tree_[parent_node.unsafe_get_just().getLabelRule()] = parent_node.unsafe_get_just();
    } else {
        tree_.erase(sub_transaction.first.second.first); // Delete the parent node
    }

    // Update or delete child nodes
    for (const auto& child : sub_transaction.second) {
        const auto& child_node = child.second;
        if (child.second.second.is_just()) {
            tree_[child.second.first] = child.second.second.unsafe_get_just(); // Update existing child node
        } else {
            tree_.erase(child.second.first); // Delete child node
        }
    }

    return true;
}
