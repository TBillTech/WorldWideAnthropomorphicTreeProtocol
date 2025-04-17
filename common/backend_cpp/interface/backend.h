#pragma once

#include <string>
#include <vector>
#include <optional>
#include <cstdint> // For uint8_t
#include <functional> // For std::function

// Represents a node in the tree structure.
struct TreeNode {
    std::string label_rule; // Unique global label rule (UTF8 encoded string)
    std::string description; // UTF8 encoded string OR a node reference
    std::optional<std::string> query_how_to; // Optional: how to query and unpack nodes
    std::optional<std::string> qa_sequence; // Optional: Q&A sequence about the node
    std::vector<std::string> literal_types; // UTF8 encoded type names
    std::optional<std::string> policy; // Optional: versioning policy
    struct Version {
        uint16_t version_number;
        uint16_t max_version_sequence;
        std::string policy;
        std::optional<std::string> authorial_proof;
        // Being author always implies being reader
        // The control idiom is that the supervisor is author at node S0
        // The write priviledge authors are authors and readers at node S1
        // The read priviledge readers are just readers at node S1
        // Thus, the authors may freely modify and change anything S2 and above
        // This also implies that generally authors cannot modify the authors list
        std::optional<std::string> authors; // Either a simple list or a node reference
        std::optional<std::string> readers;
        std::optional<int> collision_depth;
    } version;
    std::vector<std::string> child_names; // List of UTF8 label templates
    std::vector<uint8_t> contents; // Length-Value bytes
};

// For tracking transactions on nodes, each tree node modification has a prior version sequence number
// attached to the Node. 
using NewNodeVersion = std::pair<std::optional<uint16_t>, std::pair<std::string, std::optional<TreeNode>>>;
// A transactional node modification tracks the parent node, and all the descendants.  The prior version
// of the descendants is tracked for one reason:  If the transaction has a prior version that does not match
// at the time the transaction is being applied, then the transaction will fail.  However, the new
// version of the descendants is allowed to be anything at all, because it is being completely "overwritten".
using SubTransaction = std::pair<NewNodeVersion, std::vector<NewNodeVersion>>;
// A transaction is a list of subtransactions.  The transaction is atomic, meaning that all the
using Transaction = std::vector<SubTransaction>;

class Backend;

// Define a callback type for node listeners.
using NodeListenerCallback = std::function<void(Backend&, const std::string, const std::optional<TreeNode>)>;

// Pure virtual class representing the backend interface for the Tree.
class Backend {
public:
    virtual ~Backend() = default;

    // It is possible, but often not ideal to perform a raw point query.
    // This is a query that returns a single node literally matching the label rule or not.
    virtual std::optional<TreeNode> getNode(const std::string& literal_label_rule) const = 0;

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

    // The standard backend interface does not support operations on transactions, it only supports applying a 
    // transaction.  This allows for separating the concerns of building a transaction from the underlying tree functionality.
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
    virtual void notifyListeners(const std::string& label_rule, const std::optional<TreeNode>& node) = 0;
};

