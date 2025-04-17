#include "memory_tree.h"
#include "simple_backend.h"
#include "transactional_backend.h"
#include <regex>

using namespace std;

std::optional<TreeNode> TransactionalBackend::getNode(const std::string& label_rule) const {
    // If the label_rule overlaps with one of the nodes in the transaction stack,
    // then one of twp conditions should hold:
    // 1. The label_rule matches a parent node in the transaction stack
    // 2. The label_rule matches a child node in the transaction stack
    // So, test each subtransaction in the stack to see if the label_rule overlaps with any of them.
    for (const auto& sub_transaction : transaction_stack_) {
        auto parent_node_label = sub_transaction.first.second.first;
        if (checkLabelRuleOverlap(label_rule, parent_node_label)) {
            if (label_rule == parent_node_label) {
                // If the label_rule matches the parent node in the transaction stack, then return the parent node
                return sub_transaction.first.second.second;
            }
            // Not the parent node, so check each child node in the transaction stack
            for (const auto& child : sub_transaction.second) {
                // It is expected to either match a child exactly, or the label_rule is independent of the transaction so far
                if (label_rule == child.second.first) {
                    // If the label_rule matches a child node in the transaction stack, then return the child node
                    return child.second.second;
                }
            }
        }
    }
    return tree_.getNode(label_rule);
}

bool TransactionalBackend::upsertNode(const std::vector<TreeNode>& nodes) {
    if (transaction_stack_.empty()) {
        // If there is no transaction stack, then just apply the nodes to the tree
        tree_.upsertNode(nodes);
        return true;
    }
    // If there is an ongoing transaction, then all the nodes had better overlap with the transaction stack
    // Moreover, the changes will be logged to the transcation, not directly applied to the tree (yet).
    for (const auto& node : nodes) {
        // Check if the node overlaps with any of the nodes in the transaction stack
        for (auto& sub_transaction : transaction_stack_) {
            const auto& parent_node = sub_transaction.first.second.second;
            if (checkLabelRuleOverlap(node.label_rule, sub_transaction.first.second.first)) {
                // If the node overlaps with a parent node in the transaction stack, then update the transaction
                if (node.label_rule == sub_transaction.first.second.first) {
                    // If the node matches the parent node in the transaction stack, then update the parent node
                    sub_transaction.first.second.second = node;
                    return true;
                } else {
                    // Otherwise, look for an exact match to the child node
                    for (auto& child : sub_transaction.second) {
                        if (node.label_rule == child.second.first) {
                            // If the node matches a child node in the transaction stack, then update the child node
                            child.second.second = node;
                            return true;
                        }
                    }
                }
            }
        }
    }
    return true;
}

bool TransactionalBackend::deleteNode(const std::string& label_rule) {
    if (transaction_stack_.empty()) {
        // If there is no transaction stack, then just apply the delete to the tree
        tree_.deleteNode(label_rule);
        return true;
    }
    // If there is an ongoing transaction, then all the nodes had better overlap with the transaction stack
    // Moreover, the changes will be logged to the transcation, not directly applied to the tree (yet).
    for (auto& sub_transaction : transaction_stack_) {
        const auto& parent_node = sub_transaction.first.second.second;
        if (checkLabelRuleOverlap(label_rule, sub_transaction.first.second.first)) {
            // If the label_rule overlaps with a parent node in the transaction stack, then delete something
            // DANGER: deleting things in a transaction is super complex, because tracking the versions of things
            // that evaporate during the transaction is difficult.  So, we take a lazy approach for now, and assume
            // that if the user has the authority to delete, and they really want to, wiping out changes to that
            // which gets deleted is not a problem.  In other words, that deleting can delete the future accidentally.
            // In other, other words, if you want versions checked on deleted children, they need to explicitly be in the
            // transaction stack.
            if (label_rule == sub_transaction.first.second.first) {
                // If the label_rule matches the parent node in the transaction stack, then delete the parent node
                sub_transaction.first.second.second.reset();
                return true;
            } else {
                // Otherwise, look for an exact match to the child node
                for (auto& child : sub_transaction.second) {
                    if (label_rule == child.second.first) {
                        // If the label_rule matches a child node in the transaction stack, then delete the child node
                        child.second.second.reset();
                        return true;
                    }
                }
            }
            // Still didn't find it, so create a deletion for a child node in the transaction stack
            // It needs to be added to the sub_transaction.second vector
            // as a new child node with the label_rule.  
            // nullopt for the version is carved out as a special case in the transaction validity check for deletion.
            auto child_node = std::make_pair(std::nullopt, std::make_pair(label_rule, std::nullopt));
            sub_transaction.second.push_back(child_node);
            return true;
        }
    }
    throw std::runtime_error("Trying to delete a node in a transaction that is not tracked in the transaction stack");
}

std::vector<TreeNode> TransactionalBackend::queryNodes(const std::string& label_rule) const {
    if (transaction_stack_.empty()) {
        // If there is no transaction stack, then just apply the query to the tree
        return tree_.queryNodes(label_rule);
    }
    // Ideally, the query would use the transaction intelligently and in a sophisticated way.
    // However, that is super complex, since the job of interpreting the query is not the responsibility
    // of this class.  Therefore, something that should still work well is to let the backend resolve the
    // query, and then apply the transaction to the found nodes instead.
    auto nodes = tree_.queryNodes(label_rule);
    vector<TreeNode> results;
    for (const auto& node : nodes) {
        bool foundit = false;
        // Check if the node overlaps with any of the nodes in the transaction stack
        for (auto& sub_transaction : transaction_stack_) {
            const auto& parent_node = sub_transaction.first.second.second;
            if (checkLabelRuleOverlap(node.label_rule, sub_transaction.first.second.first)) {
                // If the node overlaps with a parent node in the transaction stack, push that onto the results instead
                if (node.label_rule == sub_transaction.first.second.first) {
                    // If the node matches the parent node in the transaction stack, then return that instead
                    results.push_back(sub_transaction.first.second.second.value());
                    foundit = true;
                    break;
                } else {
                    // Otherwise, look for an exact match to the child node
                    for (auto& child : sub_transaction.second) {
                        if (node.label_rule == child.second.first) {
                            // If the node matches a child node in the transaction stack, then update the child node
                            results.push_back(child.second.second.value());
                            foundit = true;
                            break;
                        }
                    }
                }
            }
        }
        if (!foundit) {
            // If the node was not found in the transaction stack, then add it to the results
            results.push_back(node);
        }
    }
    return results;
}

bool TransactionalBackend::applyTransaction(const Transaction& transaction) {
    return tree_.applyTransaction(transaction);
}

std::vector<TreeNode> TransactionalBackend::getFullTree() const {
    if (transaction_stack_.empty()) {
        // If there is no transaction stack, then just apply the query to the tree
        return tree_.getFullTree();
    }
    throw std::runtime_error("getFullTree() is not supported in transaction mode");
}


bool TransactionalBackend::openTransactionLayer(const TreeNode& node) {
    // throw an exception if this node has a label rule overlap with any other node in the transaction stack
    for (const auto& sub_transaction : transaction_stack_) {
        const auto& parent_label = sub_transaction.first.second.first;
        if (checkLabelRuleOverlap(node.label_rule, parent_label)) {
            throw std::runtime_error("Label rule overlap detected in transaction stack");
        }
    }
    // Add the new transaction to the stack
    // The initial set of descendants is empty, because the "user" will continue to add changes to the transaction
    // until the transaction is closed.
    // The old version of the node should be read from the tree, and the new version should be set to the next version
    // number of the parent node.
    auto parent_node = tree_.getNode(node.label_rule);
    if (!parent_node.has_value()) {
        // If the parent node does not exist, then the last version is nullopt
        auto parent_newnode_version = std::make_pair(std::nullopt, std::make_pair(node.label_rule, node));
        transaction_stack_.emplace_back(std::make_pair(parent_newnode_version, std::vector<NewNodeVersion>()));
    } else {
        // If the parent node exists, then use its version number as the last version
        auto next_version = (parent_node->version.version_number + 1) % (parent_node->version.max_version_sequence);
        auto parent_newnode_version = std::make_pair(parent_node->version.version_number, std::make_pair(node.label_rule, node));
        transaction_stack_.emplace_back(std::make_pair(parent_newnode_version, std::vector<NewNodeVersion>()));
    }
    return true;
}

bool TransactionalBackend::closeTransactionLayers(void) {
    // If the transaction stack is empty, then there is nothing to do
    if (transaction_stack_.empty()) {
        return true;
    }
    // Otherwise, delegate applying the transaction to the memory tree
    tree_.applyTransaction(transaction_stack_);
    // Clear the transaction stack
    transaction_stack_.clear();
    return true;
}

void TransactionalBackend::registerNodeListener(const std::string listener_name, const std::string label_rule, NodeListenerCallback callback) {
    tree_.registerNodeListener(listener_name, label_rule, callback);
}

void TransactionalBackend::deregisterNodeListener(const std::string listener_name, const std::string label_rule) {
    tree_.deregisterNodeListener(listener_name, label_rule);
}

void TransactionalBackend::notifyListeners(const std::string& label_rule, const std::optional<TreeNode>& node) {
    tree_.notifyListeners(label_rule, node);
}