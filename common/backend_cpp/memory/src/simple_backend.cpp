#include "memory_tree.h"
#include "simple_backend.h"
#include <regex>

using namespace std;
using namespace fplus;

// This function will check two nodes and verify that the label_rules to not overlap.
// label rules overlap if the label rule of one node is a prefix of the other.
// For example, "A" and "A/B" overlap, but "A/C" and "A/CB" do not.
bool checkLabelRuleOverlap(const string& label_rule_1, const string& label_rule_2) {
    auto stripQuery = [](const std::string& label_rule) {
        size_t query_pos = label_rule.find('?');
        return query_pos == std::string::npos ? label_rule : label_rule.substr(0, query_pos);
    };

    std::string stripped_label1 = stripQuery(label_rule_1);
    std::string stripped_label2 = stripQuery(label_rule_2);

    if (stripped_label1 == stripped_label2) {
        return true;
    }

    auto last_slash1 = stripped_label1.find_last_of('/');
    auto last_slash2 = stripped_label2.find_last_of('/');
    auto min_last_slash = std::min(last_slash1, last_slash2);
    // Now, if the min_last_slash is not a slash in both, then this proves that, at minimum, the
    // last label of the shorter rule has diverged from the longer rule.
    if (stripped_label1[min_last_slash] != '/' || stripped_label2[min_last_slash] != '/') {
        return false;
    }
    // On the other hand, if the strings leading up to the min_last_slash are NOT the same, then they do not overlap
    // at all, so we can return false.
    if (stripped_label1.substr(0, min_last_slash) != stripped_label2.substr(0, min_last_slash)) {
        return false;
    }
    // AT this point, it is proved that up to and including min_last_slash the rules are identical, 
    // so we consider the extensions past that point:
    auto extension1 = stripped_label1.substr(min_last_slash + 1);
    auto extension2 = stripped_label2.substr(min_last_slash + 1);
    // Now, extension1 and extension2 have the properties that the shortest label rule extension is a single
    // string not including any slash, and the other extension might have more slashes.
    // The condition where the rules overlap is also the condition that the shorter extension matches 
    // exactly with the longer extension up to that point, AND that the longer extension has a slash _after_ that.
    auto slash_pos_1 = extension1.find('/');
    auto slash_pos_2 = extension2.find('/');
    if (slash_pos_1 == std::string::npos && slash_pos_2 == std::string::npos) {
        // Since they are provably not the same at this line of code, they cannot overlap if neither has a slash at all
        return false; // No overlap
    }
    if (slash_pos_1 == std::string::npos) {
        // extension2 is longer, and it overlaps if and only if it has a slash just past the shorter extension
        return (slash_pos_2 == extension1.length());
    }
    if (slash_pos_2 == std::string::npos) {
        // extension1 is longer, and it overlaps if and only if it has a slash just past the shorter extension
        return (slash_pos_1 == extension2.length());
    }

    return false;
}

maybe<TreeNode> SimpleBackend::getNode(const std::string& label_rule) const {
    return memory_tree_.getNode(label_rule);
}

bool SimpleBackend::upsertNode(const std::vector<TreeNode>& nodes) {
    memory_tree_.upsertNode(nodes);
    // Notify listeners for each node
    for (const auto& node : nodes) {
        auto label_rule = node.getLabelRule();
        notifyListeners(label_rule, node);
    }
    return true;
}

bool SimpleBackend::deleteNode(const std::string& label_rule) {
    memory_tree_.deleteNode(label_rule);
    // Notify listeners for each node
    notifyListeners(label_rule, maybe<TreeNode>());
    return true;
}

std::vector<TreeNode> SimpleBackend::getPageTree(const std::string& page_node_label_rule) const {
    // Check if the page node exists
    auto page_node = memory_tree_.getNode(page_node_label_rule);
    if (page_node.is_nothing()) {
        throw std::runtime_error("Page node not found: " + page_node_label_rule);
    }
    // Now collect the children label rules
    auto children_label_rules = page_node.lift_def(std::vector<std::string>{}, [](const TreeNode& node) {
        return node.getChildNames();
    });
    return memory_tree_.getNodes(children_label_rules);
}

std::vector<TreeNode> SimpleBackend::relativeGetPageTree(const TreeNode& node, const std::string& page_node_label_rule) const {
    // concatenate the label rule of the node with the label rule of the page node
    std::string full_label_rule = node.getLabelRule() + "/" + page_node_label_rule;
    // Check if the page node exists
    auto page_node = memory_tree_.getNode(full_label_rule);
    if (page_node.is_nothing()) {
        throw std::runtime_error("Page node not found: " + full_label_rule);
    }
    return getPageTree(full_label_rule);
}

std::vector<TreeNode> SimpleBackend::queryNodes(const std::string& label_rule) const {
    return memory_tree_.queryNodes(label_rule);
}

std::vector<TreeNode> SimpleBackend::relativeQueryNodes(const TreeNode& node, const std::string& label_rule) const {
    // concatenate the label rule of the node with the label rule of the query
    std::string full_label_rule = node.getLabelRule() + "/" + label_rule;
    return queryNodes(full_label_rule);
}

bool SimpleBackend::openTransactionLayer(const TreeNode& node) {
    // throw an exception: SimpleBackend does not support opening Transaction layers
    throw std::runtime_error("SimpleBackend does not support opening transaction layers");
    return false;
}

bool SimpleBackend::closeTransactionLayers(void) {
    // throw an exception: SimpleBackend does not support closing Transaction layers
    throw std::runtime_error("SimpleBackend does not support closing transaction layers");
    return false;
}

bool SimpleBackend::applyTransaction(const Transaction& transaction) {
    return memory_tree_.applyTransaction(transaction);
}

std::vector<TreeNode> SimpleBackend::getFullTree() const {
    return memory_tree_.getFullTree();
}

void SimpleBackend::registerNodeListener(const std::string listener_name, const std::string label_rule, NodeListenerCallback callback) {
    auto findit = node_listeners_.find(label_rule);
    if (findit != node_listeners_.end()) {
        // If the label_rule already exists, add the listener to the existing list
        auto& listeners = findit->second.second;
        listeners.push_back(listener_name);
    } else {
        // If the label_rule does not exist, create a new entry
        node_listeners_[label_rule] = std::make_pair(callback, std::vector<std::string>{listener_name});
    }
}

void SimpleBackend::deregisterNodeListener(const std::string listener_name, const std::string label_rule) {
    // Deregister a listener for a specific label rule
    auto found = node_listeners_.find(label_rule);
    if (found != node_listeners_.end()) {
        auto& listeners = found->second.second;
        // Remove the listener from the list
        auto it = std::remove(listeners.begin(), listeners.end(), listener_name);
        if (it != listeners.end()) {
            listeners.erase(it, listeners.end());
        }
        // If the list of listeners is empty, remove the label rule from the map
        if (listeners.empty()) {
            node_listeners_.erase(found);
        }
    }
}

void SimpleBackend::notifyListeners(const std::string& label_rule, const maybe<TreeNode>& node) {
    auto found = node_listeners_.find(label_rule);
    if (found != node_listeners_.end()) {
        auto listeners = found->second.second;
        auto callback = found->second.first;
        // Notify all listeners for this label_rule
        for (auto listener : listeners) {
            callback(*this, listener, node);
        }
    }
}