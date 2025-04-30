#pragma once

#include <string>
#include <vector>
#include <cstdint> // For uint8_t
#include <fplus/fplus.hpp>
#include "shared_chunk.h"

struct TreeNodeVersion {
    uint16_t version_number;
    uint16_t max_version_sequence;
    std::string policy;
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
};

// Represents a node in the tree structure.
class TreeNode {
public:
    // Constructors
    TreeNode();
    TreeNode(const std::string& label_rule, const std::string& description, 
        const std::vector<std::string>& literal_types,
        const TreeNodeVersion& version,
        const std::vector<std::string>& child_names,
        shared_span<>&& contents, 
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

    const std::vector<std::string>& getLiteralTypes() const;
    void setLiteralTypes(const std::vector<std::string>& literal_types);

    const string& getPolicy() const;
    void setPolicy(const string& policy);

    const std::vector<std::string>& getChildNames() const;
    std::vector<std::string> getAbsoluteChildNames() const;
    void setChildNames(const std::vector<std::string>& child_names);

    const shared_span<>& getContents() const;
    void setContents(shared_span<>&& contents);

    const TreeNodeVersion& getVersion() const;
    void setVersion(const TreeNodeVersion& version);

    bool operator==(const TreeNode& other) const;
    bool operator!=(const TreeNode& other) const {
        return !(*this == other);
    }

private:
    std::string label_rule;
    std::string description;
    std::vector<std::string> literal_types;
    TreeNodeVersion version;
    std::vector<std::string> child_names;
    shared_span<> contents;
    fplus::maybe<std::string> query_how_to;
    fplus::maybe<std::string> qa_sequence;
};