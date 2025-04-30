#pragma once

#include <string>
#include <vector>
#include <cstdint> // For uint8_t
#include <functional> // For std::function
#include "tree_node.h" // Include the new header file for TreeNode

// For tracking transactions on nodes, each tree node modification has a prior version sequence number
// attached to the Node. 
using NewNodeVersion = std::pair<fplus::maybe<uint16_t>, std::pair<std::string, fplus::maybe<TreeNode>>>;
// A transactional node modification tracks the parent node, and all the descendants.  The prior version
// of the descendants is tracked for one reason:  If the transaction has a prior version that does not match
// at the time the transaction is being applied, then the transaction will fail.  However, the new
// version of the descendants is allowed to be anything at all, because it is being completely "overwritten".
using SubTransaction = std::pair<NewNodeVersion, std::vector<NewNodeVersion>>;
// A transaction is a list of subtransactions.  The transaction is atomic, meaning that all the
using Transaction = std::vector<SubTransaction>;

class Backend;

// Define a callback type for node listeners.
using NodeListenerCallback = std::function<void(Backend&, const std::string, const fplus::maybe<TreeNode>)>;
using NodeListenerCallbackArgs = std::tuple<const std::string, const fplus::maybe<TreeNode>>;

// Pure virtual class representing the backend interface for the Tree.
class Backend {
public:
    virtual ~Backend() = default;

    // It is possible, but often not ideal to perform a raw point query.
    // This is a query that returns a single node literally matching the label rule or not.
    virtual fplus::maybe<TreeNode> getNode(const std::string& literal_label_rule) const = 0;

    // Add or update a parent node and its children in the tree.
    virtual bool upsertNode(const std::vector<TreeNode>& nodes) = 0;

    // Delete a node and its children from the tree.
    virtual bool deleteNode(const std::string& label_rule) = 0;

    // Retreive a whole subtree according to the label rules listed in a page node.
    // The page node is a special node that contains a list of label rules.
    // The subtree is returned as a vector of TreeNode objects.
    virtual std::vector<TreeNode> getPageTree(const std::string& page_node_label_rule) const = 0;
    virtual std::vector<TreeNode> relativeGetPageTree(const TreeNode& node, const std::string& page_node_label_rule) const = 0;

    // Query nodes matching a label rule.
    virtual std::vector<TreeNode> queryNodes(const std::string& label_rule) const = 0;
    virtual std::vector<TreeNode> relativeQueryNodes(const TreeNode& node, const std::string& label_rule) const = 0;

    virtual bool openTransactionLayer(const TreeNode& node) = 0;
    virtual bool closeTransactionLayers(void) = 0;
    virtual bool applyTransaction(const Transaction& transaction) = 0;

    // Retrieve the entire tree structure (for debugging or full sync purposes).  Obviously, this will not
    // return nodes that the user does not have permission to read.
    virtual std::vector<TreeNode> getFullTree() const = 0;

    // Register and deregister node listeners.  These proc when a node version is modified (or deleted).
    // For token ring nodes, the listener will get notified only after the token has been passed to it.
    // Technically, this means you cannot listen to sub components of data-structures.
    // Which also means that implementations should not modify data blocks without updating the version.
    virtual void registerNodeListener(const std::string listener_name, const std::string label_rule, NodeListenerCallback callback) = 0;
    virtual void deregisterNodeListener(const std::string listener_name, const std::string label_rule) = 0;

    // Sometimes a higher level backend needs to tell a lower level backend to notify a listener of something,
    // even though the higher level backend is not explicitly tracking the listeners. So, notifyListeners is in this interface:
    virtual void notifyListeners(const std::string& label_rule, const fplus::maybe<TreeNode>& node) = 0;

    // Process one notification for a specific label rule (if the backend supports it).  This is used to process notifications in a worker thread.
    virtual void processNotification() = 0;
};

