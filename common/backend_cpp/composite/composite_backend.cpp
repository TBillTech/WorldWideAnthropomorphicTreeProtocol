#include "composite_backend.h"
#include "simple_backend.h"

fplus::maybe<TreeNode> CompositeBackend::getNode(const std::string& label_rule) const {
    auto index_backend = getRelevantBackend(label_rule);
    fplus::maybe<TreeNode> result = index_backend.second.getNode(label_rule.substr(index_backend.first));
    if (result.is_nothing()) {
        return result;
    } else {
        result.unsafe_get_just().setLabelRule(label_rule.substr(0, index_backend.first) + '/' + result.unsafe_get_just().getLabelRule());
    }
    return result;
}

bool CompositeBackend::upsertNode(const std::vector<TreeNode>& nodes) {
    // Since we have to duplicate the nodes to modify the relative label rules, do them one by one.
    for (const auto& node : nodes) {
        auto index_backend = getRelevantBackend(node.getLabelRule());
        auto new_node = node;
        new_node.setLabelRule(node.getLabelRule().substr(index_backend.first));
        index_backend.second.upsertNode({new_node});
    }
    return true;
}

bool CompositeBackend::deleteNode(const std::string& label_rule) {
    auto index_backend = getRelevantBackend(label_rule);
    return index_backend.second.deleteNode(label_rule.substr(index_backend.first));
}

std::vector<TreeNode> CompositeBackend::getPageTree(const std::string& page_node_label_rule) const {
    auto index_backend = getRelevantBackend(page_node_label_rule);
    auto result = index_backend.second.getPageTree(page_node_label_rule.substr(index_backend.first));
    for (auto& node : result) {
        node.setLabelRule(page_node_label_rule.substr(0, index_backend.first) + '/' + node.getLabelRule());
    } 
    return result;
}

std::vector<TreeNode> CompositeBackend::relativeGetPageTree(const TreeNode& node, const std::string& page_node_label_rule) const {
    auto absolute_label_rule = node.getLabelRule() + '/' + page_node_label_rule;
    return getPageTree(page_node_label_rule);
}

std::vector<TreeNode> CompositeBackend::queryNodes(const std::string& label_rule) const {
    auto index_backend = getRelevantBackend(label_rule);
    auto result = index_backend.second.queryNodes(label_rule.substr(index_backend.first));
    for (auto& node : result) {
        node.setLabelRule(label_rule.substr(0, index_backend.first) + '/' + node.getLabelRule());
    }
    return result;
}

std::vector<TreeNode> CompositeBackend::relativeQueryNodes(const TreeNode& node, const std::string& label_rule) const {
    auto absolute_label_rule = node.getLabelRule() + '/' + label_rule;
    return queryNodes(absolute_label_rule);
}

bool CompositeBackend::openTransactionLayer(const TreeNode& node) {
    auto index_backend = getRelevantBackend(node.getLabelRule());
    auto new_node = node;
    new_node.setLabelRule(node.getLabelRule().substr(index_backend.first));
    return index_backend.second.openTransactionLayer(new_node);
}

bool CompositeBackend::closeTransactionLayers(void) {
    // Close all transaction layers in the mounted backends
    for (const auto& [label_rule, backend] : mounted_backends_) {
        backend.closeTransactionLayers();
    }
    return root_backend_.closeTransactionLayers();
}

bool CompositeBackend::applyTransaction(const Transaction& transaction) {
    // First check if all the SubTransactions are for the same backend.
    // If they are not, throw an exception.
    auto index_backend = getRelevantBackend(transaction.begin()->first.second.first);
    auto prefix = transaction.begin()->first.second.first.substr(0, index_backend.first);
    mounted_transaction.clear(); 
    for (auto sub_transaction : transaction) {
        auto sub_index_backend = getRelevantBackend(sub_transaction.first.second.first);
        auto sub_prefix = sub_transaction.first.second.first.substr(0, sub_index_backend.first);
        if (sub_prefix != prefix) {
            throw std::runtime_error("Transaction contains SubTransactions for different backends");
        }
        auto relative_label_rule = sub_transaction.first.second.first.substr(sub_index_backend.first);
        mounted_transaction.push_back({{sub_transaction.first.first, {relative_label_rule, sub_transaction.first.second.second}}, sub_transaction.second});
    }

    return index_backend.second.applyTransaction(mounted_transaction);
}

std::vector<TreeNode> CompositeBackend::getFullTree() const {
    std::vector<TreeNode> full_tree = root_backend_.getFullTree();
    for (const auto& [label_rule, backend] : mounted_backends_) {
        auto relative_tree = backend.getFullTree();
        for (auto& node : relative_tree) {
            node.setLabelRule(label_rule + '/' + node.getLabelRule());
        }
        full_tree.insert(full_tree.end(), relative_tree.begin(), relative_tree.end());
    }
    return full_tree;
}

void CompositeBackend::registerNodeListener(const std::string listener_name, const std::string label_rule, NodeListenerCallback callback) {
    auto index_backend = getRelevantBackend(label_rule);
    auto relative_label_rule = label_rule.substr(index_backend.first);
    index_backend.second.registerNodeListener(listener_name, relative_label_rule, callback);
}

void CompositeBackend::deregisterNodeListener(const std::string listener_name, const std::string label_rule) {
    auto index_backend = getRelevantBackend(label_rule);
    auto relative_label_rule = label_rule.substr(index_backend.first);
    root_backend_.deregisterNodeListener(listener_name, relative_label_rule);
}

void CompositeBackend::notifyListeners(const std::string& label_rule, const fplus::maybe<TreeNode>& node) {
    auto index_backend = getRelevantBackend(label_rule);
    auto relative_label_rule = label_rule.substr(index_backend.first);
    if (node.is_nothing()) {
        index_backend.second.notifyListeners(relative_label_rule, node);
        return;
    }
    auto node_copy = node.unsafe_get_just();
    node_copy.setLabelRule(node_copy.getLabelRule().substr(index_backend.first));
    index_backend.second.notifyListeners(relative_label_rule, node);
}

void CompositeBackend::processNotification() {
    root_backend_.processNotification();
    for (const auto& [label_rule, backend] : mounted_backends_) {
        backend.processNotification();
    }
}

std::pair<std::map<std::string, Backend &>::iterator, bool> CompositeBackend::mountBackend(const std::string& label_rule, Backend& backend) {
    if (mounted_backends_.find(label_rule) != mounted_backends_.end()) {
        throw std::runtime_error("Label rule already exists in the mounted backends");
    }
    if (root_backend_.getNode(label_rule).is_just()) {
        throw std::runtime_error("Label rule already exists in the root backend");
    }
    // Check for overlapping label rules
    for (const auto& [mounted_label_rule, mounted_backend] : mounted_backends_) {
        if (checkLabelRuleOverlap(label_rule, mounted_label_rule)) {
            throw std::runtime_error("Label rule overlaps with an existing mounted backend");
        }
    }
    return mounted_backends_.insert(std::pair<std::string, Backend&>(label_rule, backend));
}

pair<int, Backend&> CompositeBackend::getRelevantBackend(const std::string& label_rule) const {
    // Assume that no two backends can have overlapping label rules.
    // Therefore it is safe to assume that the label_rule can only overlap one or none of the mounted backends.
    // This also means that the relevant backend is one that matches the label_rule up to the size of the mounted_label_rule.
    // Furthermore, since the map is sorted by string, we can look for the upper_bound of the label_rule in the map, and 
    // then decrement once to find the _only_ possible relevant backend.
    auto it = mounted_backends_.upper_bound(label_rule);
    if (it == mounted_backends_.begin()) {
        throw std::runtime_error("No relevant backend found for label rule: " + label_rule);
    }
    --it;
    if (label_rule.compare(0, it->first.size(), it->first) == 0) {
        return {it->first.size(), it->second};
    }
    return {0, root_backend_};
}
