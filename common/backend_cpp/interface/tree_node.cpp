#include "tree_node.h"

using namespace std;

TreeNode::TreeNode() 
    : label_rule(""), description(""),
      literal_types(), child_names(), contents(global_no_chunk_header, false), 
      version({0, 0, ""}) {}

TreeNode::TreeNode(const std::string& label_rule, const std::string& description, 
        const std::vector<std::string>& literal_types,
        const TreeNodeVersion& version,
        const std::vector<std::string>& child_names,
        shared_span<>&& contents, 
        const fplus::maybe<std::string>& query_how_to,
        const fplus::maybe<std::string>& qa_sequence)
    : label_rule(label_rule), description(description), literal_types(literal_types),
      version(version), child_names(child_names), contents(move(contents)), 
      query_how_to(query_how_to), qa_sequence(qa_sequence)
    {}

// The assignment operators are for the most part typical, except for the handling of the contents.
// When doing move semantics, the contents need to use move semantics.
// When doing copy semantics, the contents need to use the shared_span restrict method to get a reference counted view of the data.
TreeNode& TreeNode::operator=(const TreeNode& other) {
    if (this != &other) {
        label_rule = other.label_rule;
        description = other.description;
        literal_types = other.literal_types;
        child_names = other.child_names;
        auto content_total_range = make_pair(0, other.contents.size());
        contents = move(other.contents.restrict(content_total_range));
        version = other.version;
        query_how_to = other.query_how_to;
        qa_sequence = other.qa_sequence;
    }
    return *this;
}

TreeNode& TreeNode::operator=(TreeNode&& other) noexcept {
    if (this != &other) {
        label_rule = std::move(other.label_rule);
        description = std::move(other.description);
        literal_types = std::move(other.literal_types);
        child_names = std::move(other.child_names);
        contents = std::move(other.contents);
        version = std::move(other.version);
        query_how_to = std::move(other.query_how_to);
        qa_sequence = std::move(other.qa_sequence);
    }
    return *this;
}

TreeNode::TreeNode(TreeNode&& other) noexcept
    : label_rule(std::move(other.label_rule)), description(std::move(other.description)),
      literal_types(std::move(other.literal_types)),
      child_names(std::move(other.child_names)), contents(std::move(other.contents)),
      version(std::move(other.version)),
      query_how_to(std::move(other.query_how_to)), qa_sequence(std::move(other.qa_sequence))
      {}

TreeNode::TreeNode(const TreeNode& other)
    : label_rule(other.label_rule), description(other.description),
      literal_types(other.literal_types),
      child_names(other.child_names), contents(other.contents),
      version(other.version), 
      query_how_to(other.query_how_to), qa_sequence(other.qa_sequence)
      {}

const std::string& TreeNode::getLabelRule() const {
    return label_rule;
}

void TreeNode::setLabelRule(const std::string& label_rule) {
    this->label_rule = label_rule;
}

const std::string& TreeNode::getDescription() const {
    return description;
}

void TreeNode::setDescription(const std::string& description) {
    this->description = description;
}

const fplus::maybe<std::string>& TreeNode::getQueryHowTo() const {
    return query_how_to;
}

void TreeNode::setQueryHowTo(const fplus::maybe<std::string>& query_how_to) {
    this->query_how_to = query_how_to;
}

const fplus::maybe<std::string>& TreeNode::getQaSequence() const {
    return qa_sequence;
}

void TreeNode::setQaSequence(const fplus::maybe<std::string>& qa_sequence) {
    this->qa_sequence = qa_sequence;
}

const std::vector<std::string>& TreeNode::getLiteralTypes() const {
    return literal_types;
}

void TreeNode::setLiteralTypes(const std::vector<std::string>& literal_types) {
    this->literal_types = literal_types;
}

const string& TreeNode::getPolicy() const {
    return version.policy;
}

void TreeNode::setPolicy(const string& policy) {
    this->version.policy = policy;
}

const std::vector<std::string>& TreeNode::getChildNames() const {
    return child_names;
}

std::vector<std::string> TreeNode::getAbsoluteChildNames() const {
    std::vector<std::string> absolute_child_names;
    for (const auto& child_name : child_names) {
        absolute_child_names.push_back(label_rule + "/" + child_name);
    }
    return move(absolute_child_names);
}

void TreeNode::setChildNames(const std::vector<std::string>& child_names) {
    this->child_names = child_names;
}

const shared_span<>& TreeNode::getContents() const {
    return contents;
}

void TreeNode::setContents(shared_span<>&& contents) {
    this->contents = move(contents);
}

const TreeNodeVersion& TreeNode::getVersion() const {
    return version;
}

void TreeNode::setVersion(const TreeNodeVersion& version) {
    this->version = version;
}

bool TreeNode::operator==(const TreeNode& other) const {
    bool properties_equal = label_rule == other.label_rule &&
           description == other.description &&
           literal_types == other.literal_types &&
           version == other.version &&
           child_names == other.child_names &&
           query_how_to == other.query_how_to &&
           qa_sequence == other.qa_sequence;
    bool contents_equal = contents.size() == other.contents.size();
    if (contents_equal) {
        auto this_it = contents.begin<uint8_t>();
        auto other_it = other.contents.begin<uint8_t>();
        for (size_t i = 0; i < contents.size(); ++i) {
            if (*this_it != *other_it) {
                contents_equal = false;
                break;
            }
            ++this_it;
            ++other_it;
        }
    }
    return properties_equal && contents_equal;
}