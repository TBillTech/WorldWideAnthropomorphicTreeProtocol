#include "backend_testbed.h"
#include <catch2/catch_test_macros.hpp>

TreeNode createNoContentTreeNode(string label_rule, string description, vector<string> literal_types, 
    TreeNodeVersion version, vector<string> child_names, 
    maybe<string> query_how_to, maybe<string> qa_sequence) {
    shared_span<> no_content(global_no_chunk_header, false);
    return TreeNode(label_rule, description, literal_types, version, child_names, std::move(no_content), std::move(query_how_to), std::move(qa_sequence));
}

TreeNode createAnimalNode(string animal, string description, vector<string> literal_types, 
    TreeNodeVersion version, vector<string> child_names, 
    vector<pair<int, string>> contents, string query_how_to, string qa_sequence) {
    shared_span<> animal_data(global_no_chunk_header, true);
    pair<bool, pair<size_t, size_t>> next = {false, {0, 0}};
    for (auto& content : contents) {
        next = {true, animal_data.copy_type<int>(content.first, next)};
        auto a_string = content.second;
        next.second = animal_data.copy_type<int>(a_string.size(), next);
        next.second = animal_data.copy_span<const char>(std::span<const char>(a_string.c_str(), a_string.size()), next);
    }
    auto only_animal_data = animal_data.restrict_upto(next.second);
    auto m_query_how_to = maybe<string>(query_how_to);
    REQUIRE(m_query_how_to.get_with_default("") == query_how_to);
    auto m_qa_sequence = maybe<string>(qa_sequence);
    REQUIRE(m_qa_sequence.get_with_default("") == qa_sequence);
    return TreeNode(animal, description, literal_types, version, child_names, std::move(only_animal_data), m_query_how_to, m_qa_sequence);
}

vector<TreeNode> createAnimalDossiers(TreeNode &animal_node) {
    vector<TreeNode> animal_dossiers;
    for (const auto& child_name : animal_node.getChildNames()) {
        string child_label_rule = animal_node.getLabelRule() + "/" + child_name;
        TreeNodeVersion child_version = {1, 0, "public", maybe<string>(), maybe<string>(), maybe<string>(), maybe<int>(2)};
        TreeNode child_node = createNoContentTreeNode(child_label_rule, "Animal Dossier", {}, child_version, {}, maybe<string>(), maybe<string>());
        vector<string> note_names;
        for (int i = 0; i < 3; ++i) {
            string note_label_rule = child_label_rule + "/note" + std::to_string(i);
            note_names.push_back("note" + std::to_string(i));
            TreeNodeVersion note_version = {1, 0, "public", maybe<string>(), maybe<string>(), maybe<string>(), maybe<int>(2)};
            TreeNode note_node = createNoContentTreeNode(note_label_rule, child_name + " note " + std::to_string(i), {}, note_version, {}, maybe<string>(), maybe<string>());
            animal_dossiers.push_back(std::move(note_node));
        }
        child_node.setChildNames(note_names);
        animal_dossiers.push_back(std::move(child_node));
    }
    return animal_dossiers;
}

vector<TreeNode> createLionNodes() {
    TreeNode lion = createAnimalNode(
        "lion",
        "King of the jungle", 
        {"integer", "string"},
        {1, 0, "public", maybe<string>(), maybe<string>("tester"), maybe<string>("tester"), maybe<int>(2)}, 
        {"Simba", "Nala"},
        {{10, "carnivore"}},
        "url duh!", 
        "Zookeeper1: Lions are majestic.\nZookeeper2: Indeed, they are the kings of the jungle."
    );
    vector<TreeNode> lion_nodes = createAnimalDossiers(lion);
    lion_nodes.insert(lion_nodes.begin(), lion);
    return lion_nodes;
}

vector<TreeNode> createElephantNodes() {
    TreeNode elephant = createAnimalNode(
        "elephant", 
        "Largest land animal", 
        {"integer", "string"}, 
        {1, 0, "public", maybe<string>(), maybe<string>(), maybe<string>(), maybe<int>(2)}, 
        {"Dumbo", "Babar"}, 
        {{8, "herbivore"}},
        "url duh!", 
        "Zookeeper1: Elephants are so strong.\nZookeeper2: And they have great memory!"
    );
    vector<TreeNode> elephant_nodes = createAnimalDossiers(elephant);
    elephant_nodes.insert(elephant_nodes.begin(), elephant);
    if (elephant.getQueryHowTo().get_with_default("") != "url duh!") {
        std::cerr << "Elephant node has incorrect qa_sequence" << std::endl;
        throw std::runtime_error("Elephant node has incorrect qa_sequence");
    }
    return elephant_nodes;
}

vector<TreeNode> createParrotNodes() {
    TreeNode parrot = createAnimalNode(
        "parrot", 
        "Colorful bird", 
        {"integer", "string"}, 
        {1, 0, "public", maybe<string>(), maybe<string>(), maybe<string>(), maybe<int>(2)}, 
        {"Polly", "Jerome"}, 
        {{7, "omnivore"}},
        "url duh!", 
        "Zookeeper1: Parrots can mimic sounds.\nZookeeper2: Yes, they are very intelligent birds."
    );
    vector<TreeNode> parrot_nodes = createAnimalDossiers(parrot);
    parrot_nodes.insert(parrot_nodes.begin(), parrot);
    return parrot_nodes;
}

void checkGetNode(Backend const &backend, const string& label_rule, TreeNode const &expected_node) {
    maybe<TreeNode> node = backend.getNode(label_rule);
    if (node.is_just()) {
        auto found_node = node.get_with_default(TreeNode());
        REQUIRE(found_node.getLabelRule() == expected_node.getLabelRule());
        REQUIRE(found_node == expected_node);
    } else {
        FAIL("Node not found: " + label_rule);
    }
}

void checkMultipleGetNode(Backend const &backend, const vector<TreeNode>& expected_nodes) {
    for (auto& expected_node : expected_nodes) {
        checkGetNode(backend, expected_node.getLabelRule(), expected_node);
    }
}

void checkDeletedNode(Backend const &backend, const string& label_rule) {
    auto node = backend.getNode(label_rule);
    REQUIRE(node.is_nothing());
}

void checkMultipleDeletedNode(Backend const &backend, const vector<TreeNode>& expected_nodes) {
    for (auto& expected_node : expected_nodes) {
        checkDeletedNode(backend, expected_node.getLabelRule());
    }
}

vector<TreeNode> collectAllNotes() {
    auto lion_nodes = createLionNodes();
    auto elephant_nodes = createElephantNodes();
    auto parrot_nodes = createParrotNodes();
    vector<TreeNode> all_notes;
    for (auto& lion_node : lion_nodes) {
        if (lion_node.getLabelRule().find("note") != string::npos) {
            all_notes.push_back(lion_node);
        }
    }
    for (auto& elephant_node : elephant_nodes) {
        if (elephant_node.getLabelRule().find("note") != string::npos) {
            all_notes.push_back(elephant_node);
        }
    }
    for (auto& parrot_node : parrot_nodes) {
        if (parrot_node.getLabelRule().find("note") != string::npos) {
            all_notes.push_back(parrot_node);
        }
    }
    return all_notes;
}

TreeNode createNotesPageTree() {
    auto noteNodes = collectAllNotes();
    vector<string> note_labels;
    for (auto& note : noteNodes) {
        note_labels.push_back(note.getLabelRule());
    }
    TreeNode notes_page_tree = createNoContentTreeNode("notes", "Animal Notes Page", {}, 
        {1, 0, "public", maybe<string>(), maybe<string>(), maybe<string>(), maybe<int>(2)}, 
        note_labels, maybe<string>(), maybe<string>());
    return notes_page_tree;
}

vector<TreeNode> prefixNodeLabels(string label_prefix, vector<TreeNode> nodes) {
    for (auto& node : nodes) {
        node.setLabelRule(label_prefix + node.getLabelRule());
        auto child_names = node.getChildNames();
        for (auto& child_name : child_names) {
            child_name = label_prefix + child_name;
        }
        node.setChildNames(child_names);
    }
    return nodes;
}

BackendTestbed::BackendTestbed(Backend& backend, bool should_test_notifications) 
    : backend(backend), should_test_notifications_(should_test_notifications) {}

void BackendTestbed::addAnimalsToBackend() {
    backend.upsertNode(createLionNodes());
    backend.upsertNode(createElephantNodes());
    backend.upsertNode(createParrotNodes());
}

void BackendTestbed::addNotesPageTree() {
    backend.upsertNode({createNotesPageTree()});
}

void BackendTestbed::testBackendLogically(string label_prefix) {
    checkMultipleGetNode(backend, prefixNodeLabels(label_prefix, createLionNodes()));
    checkMultipleGetNode(backend, prefixNodeLabels(label_prefix, createElephantNodes()));
    checkMultipleGetNode(backend, prefixNodeLabels(label_prefix, createParrotNodes()));

    auto notes_pages = backend.getPageTree(label_prefix + "notes");
    REQUIRE(notes_pages == prefixNodeLabels(label_prefix, collectAllNotes()));

    {
        MemoryTree temp_tree;
        SimpleBackend temp_backend(temp_tree);
        temp_backend.upsertNode(backend.getFullTree());
        checkMultipleGetNode(temp_backend, prefixNodeLabels(label_prefix, createElephantNodes()));
        checkMultipleGetNode(temp_backend, prefixNodeLabels(label_prefix, createLionNodes()));
        checkMultipleGetNode(temp_backend, prefixNodeLabels(label_prefix, createParrotNodes()));
        auto notes_pages = temp_backend.getPageTree(label_prefix + "notes");
        REQUIRE(notes_pages == prefixNodeLabels(label_prefix, collectAllNotes()));
    }
    
    backend.deleteNode(label_prefix + "elephant");
    checkMultipleDeletedNode(backend, prefixNodeLabels(label_prefix, createElephantNodes()));
    checkMultipleGetNode(backend, prefixNodeLabels(label_prefix, createLionNodes()));
    checkMultipleGetNode(backend, prefixNodeLabels(label_prefix, createParrotNodes()));

    if (!should_test_notifications_) {
        return;
    }

    bool lion_node_created = false;
    bool lion_node_deleted = false;
    string lion_label = label_prefix + "lion";
    backend.registerNodeListener("lion_listener", lion_label, false, [lion_label, &lion_node_created, &lion_node_deleted](Backend& backend, const string listener_name, const fplus::maybe<TreeNode> node) {
        if (node.is_just()) {
            auto found_node = node.get_with_default(TreeNode());
            if (found_node.getLabelRule() == lion_label) {
                lion_node_created = true;
            }
        } else {
            lion_node_deleted = true;
        }
    });
    backend.upsertNode(prefixNodeLabels(label_prefix, createLionNodes()));
    backend.processNotification();
    REQUIRE(lion_node_created);
    backend.deleteNode(lion_label);
    backend.processNotification();
    REQUIRE(lion_node_deleted);
    backend.deregisterNodeListener("lion_listener", lion_label);
    lion_node_created = false;
    lion_node_deleted = false;
    backend.upsertNode(prefixNodeLabels(label_prefix, createLionNodes()));
    backend.processNotification();
    REQUIRE(!lion_node_created);
    backend.deleteNode(lion_label);
    backend.processNotification();
    REQUIRE(!lion_node_deleted);
}
