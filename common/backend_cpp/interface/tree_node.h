#pragma once

#include <string>
#include <vector>
#include <cstdint> // For uint8_t
#include <fplus/fplus.hpp>
#include "shared_chunk.h"

// operators for reading and writing a fplus::maybe<T> to a stream
namespace fplus {
    template<typename T>
    std::ostream& operator<<(std::ostream& os, const maybe<T>& m) {
        if (m.is_just()) {
            os << "Just " << m.get_with_default(T());
        } else {
            os << "Nothing";
        }
        return os;
    }

    template<typename T>
    std::istream& operator>>(std::istream& is, maybe<T>& m) {
        std::string str;
        is >> str;
        if (str == "Nothing") {
            m = maybe<T>();
        } else {
            T value;
            is >> value;
            m = just(value);
        }
        return is;
    }
}

void write_length_string(std::ostream& os, const std::string& label, const std::string& str);
pair<string,string> read_length_string(std::istream& is);

std::ostream& hide_contents(std::ostream& os);
std::ostream& show_contents(std::ostream& os);

class TreeNodeVersion {
    public:
        uint16_t version_number;
        uint16_t max_version_sequence;
        std::string policy = "default";
        fplus::maybe<std::string> authorial_proof;
        fplus::maybe<std::string> authors;
        fplus::maybe<std::string> readers;
        fplus::maybe<int> collision_depth;

        bool operator==( const TreeNodeVersion& other ) const {
            return version_number == other.version_number &&
                max_version_sequence == other.max_version_sequence &&
                policy == other.policy &&
                authorial_proof == other.authorial_proof &&
                authors == other.authors &&
                readers == other.readers &&
                collision_depth == other.collision_depth;
        }
        friend std::ostream& operator<<(std::ostream& os, const TreeNodeVersion& version) {
            if (version.policy.empty()) {
                throw std::runtime_error("TreeNodeVersion policy cannot be empty.  'default' should be used at minimum.");
            }
            os << "Version: " << version.version_number << " Max_Version: " << version.max_version_sequence
            << " Policy: " << version.policy << " Authorial_Proof: " << version.authorial_proof
            << " Authors: " << version.authors << " Readers: " << version.readers
            << " Collision_Depth: " << version.collision_depth << " ";
            return os;
        }
        friend std::istream& operator>>(std::istream& is, TreeNodeVersion& version) {
            std::string label;
            is >> label >> version.version_number;
            is >> label >> version.max_version_sequence;
            is >> label >> version.policy;
            is >> label >> version.authorial_proof;
            is >> label >> version.authors;
            is >> label >> version.readers;
            is >> label >> version.collision_depth;
            is.get(); // consume the space
            return is;
        }
};

// Represents a node in the tree structure.
class TreeNode {
public:
    using PropertyInfo = std::pair<std::string, std::string>; // Type and name of the property

    // Constructors
    TreeNode();
    TreeNode(const std::string& label_rule, const std::string& description, 
        const std::vector<PropertyInfo>& property_infos,
        const TreeNodeVersion& version,
        const std::vector<std::string>& child_names,
        shared_span<>&& property_data, 
        const fplus::maybe<std::string>& query_how_to,
        const fplus::maybe<std::string>& qa_sequence);

    // Assignment operator
    TreeNode& operator=(const TreeNode& other);
    TreeNode& operator=(TreeNode&& other) noexcept;
    TreeNode(TreeNode&& other) noexcept;
    TreeNode(const TreeNode& other);
    ~TreeNode() = default;

    // Getters and setters
    const std::string& getLabelRule() const;
    void setLabelRule(const std::string& label_rule);

    const std::string& getDescription() const;
    void setDescription(const std::string& description);

    const fplus::maybe<std::string>& getQueryHowTo() const;
    void setQueryHowTo(const fplus::maybe<std::string>& query_how_to);

    const fplus::maybe<std::string>& getQaSequence() const;
    void setQaSequence(const fplus::maybe<std::string>& qa_sequence);

    const std::vector<PropertyInfo>& getPropertyInfo() const;
    void setPropertyInfo(const std::vector<PropertyInfo>& property_infos);

    const string& getPolicy() const;
    void setPolicy(const string& policy);

    const std::vector<std::string>& getChildNames() const;
    std::vector<std::string> getAbsoluteChildNames() const;
    void setChildNames(const std::vector<std::string>& child_names);

    const shared_span<>& getPropertyData() const;
    void setPropertyData(shared_span<>&& property_data);

    const TreeNodeVersion& getVersion() const;
    void setVersion(const TreeNodeVersion& version);

    bool operator==(const TreeNode& other) const;
    bool operator!=(const TreeNode& other) const {
        return !(*this == other);
    }

    // Operators for reading and writing to a stream
    friend std::ostream& operator<<(std::ostream& os, const TreeNode& node);
    friend std::istream& operator>>(std::istream& is, TreeNode& node);

    void prefixLabels(const std::string& prefix);
    void shortenLabels(const std::string& prefix);

private:
    std::string label_rule;
    std::string description;
    std::vector<PropertyInfo> property_infos;
    TreeNodeVersion version;
    std::vector<std::string> child_names;
    shared_span<> property_data;
    fplus::maybe<std::string> query_how_to;
    fplus::maybe<std::string> qa_sequence;
};

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

void prefixTransactionLabels(const std::string& prefix, Transaction& transaction);
void shortenTransactionLabels(const std::string& prefix, Transaction& transaction);

void prefixSubTransactionLabels(const std::string& prefix, SubTransaction& sub_transaction);
void shortenSubTransactionLabels(const std::string& prefix, SubTransaction& sub_transaction);

void prefixNewNodeVersionLabels(const std::string& prefix, NewNodeVersion& new_node_version);
void shortenNewNodeVersionLabels(const std::string& prefix, NewNodeVersion& new_node_version);