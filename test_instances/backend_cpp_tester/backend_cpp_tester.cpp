#include "simple_backend.h"
#include "threadsafe_backend.h"
#include "transactional_backend.h"
#include "composite_backend.h"
#include <iostream>
#include <thread>
#include <vector>

using namespace std;
using namespace fplus;

TreeNode createNoContentTreeNode(string label_rule, string description, vector<string> literal_types, 
    TreeNodeVersion version, vector<string> child_names, 
    maybe<string> query_how_to, maybe<string> qa_sequence) {
    shared_span<> no_content(global_no_chunk_header, false);
    return TreeNode(label_rule, description, literal_types, version, child_names, move(no_content), move(query_how_to), move(qa_sequence));
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
    if (m_query_how_to.get_with_default("") != query_how_to) {
        cerr << "Animal node has incorrect query_how_to" << endl;
        throw runtime_error("Animal node has incorrect query_how_to");
    }
    auto m_qa_sequence = maybe<string>(qa_sequence);
    if (m_qa_sequence.get_with_default("") != qa_sequence) {
        cerr << "Animal node has incorrect qa_sequence" << endl;
        throw runtime_error("Animal node has incorrect qa_sequence");
    }
    return TreeNode(animal, description, literal_types, version, child_names, move(only_animal_data), m_query_how_to, m_qa_sequence);
}

vector<TreeNode> createAnimalDossiers(TreeNode &animal_node) {
    // For each child_name, create a TreeNode with label_rule/child_name and some information about the animal
    vector<TreeNode> animal_dossiers;
    for (const auto& child_name : animal_node.getChildNames()) {
        string child_label_rule = animal_node.getLabelRule() + "/" + child_name;
        TreeNodeVersion child_version = {1, 0, "public", maybe<string>(), maybe<string>(), maybe<string>(), maybe<int>(2)};
        TreeNode child_node = createNoContentTreeNode(child_label_rule, "Animal Dossier", {}, child_version, {}, maybe<string>(), maybe<string>());
        // Also add one to three notes to each animal dossier
        vector<string> note_names;
        for (int i = 0; i < 3; ++i) {
            string note_label_rule = child_label_rule + "/note" + to_string(i);
            note_names.push_back("note" + to_string(i));
            TreeNodeVersion note_version = {1, 0, "public", maybe<string>(), maybe<string>(), maybe<string>(), maybe<int>(2)};
            TreeNode note_node = createNoContentTreeNode(note_label_rule, child_name + " note " + to_string(i), {}, note_version, {}, maybe<string>(), maybe<string>());
            animal_dossiers.push_back(move(note_node));
        }
        child_node.setChildNames(note_names);
        animal_dossiers.push_back(move(child_node));
    }
    return animal_dossiers;
}

vector<TreeNode> createLionNodes()
{
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

vector<TreeNode> createElephantNodes()
{
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
        cerr << "Elephant node has incorrect qa_sequence" << endl;
        throw runtime_error("Elephant node has incorrect qa_sequence");
    }
    return elephant_nodes;
}

vector<TreeNode> createParrotNodes()
{
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

void addAnimalsToBackend(Backend &backend)
{
    // Add the lion and its dossiers to the backend
    backend.upsertNode(createLionNodes());

    backend.upsertNode(createElephantNodes());

    backend.upsertNode(createParrotNodes());
}

void checkGetNode(Backend &backend, const string& label_rule, TreeNode const &expected_node) {
    auto node = backend.getNode(label_rule);
    if (node.is_just()) {
        auto found_node = node.get_with_default(TreeNode());
        if (found_node.getLabelRule() == expected_node.getLabelRule()) {
            cout << "Correct Node found: " << found_node.getLabelRule() << " matched expected: " << expected_node.getLabelRule() << endl;
        } else {
            cerr << "Node mismatch: " << found_node.getLabelRule() << " != " << expected_node.getLabelRule() << endl;
            throw runtime_error("Node mismatch looking for: " + label_rule);
        }
        if (found_node != expected_node) {
            cerr << "Node mismatch for: " << label_rule << endl;
            throw runtime_error("Node content mismatch looking for: " + label_rule);
        }
    } else {
        cerr << "Node not found: " << label_rule << endl;
        throw runtime_error("Node not found looking for: " + label_rule);
    }
}

void checkMultipleGetNode(Backend &backend, const vector<TreeNode>& expected_nodes) {
    for (auto& expected_node : expected_nodes) {
        checkGetNode(backend, expected_node.getLabelRule(), expected_node);
    }
}

void checkDeletedNode(Backend &backend, const string& label_rule) {
    auto node = backend.getNode(label_rule);
    if (node.is_just()) {
        cerr << "Node not deleted: " << label_rule << endl;
        throw runtime_error("Node not deleted looking for: " + label_rule);
    } else {
        cout << "Node deleted: " << label_rule << endl;
    }
}

void checkMultipleDeletedNode(Backend &backend, const vector<TreeNode>& expected_nodes) {
    for (auto& expected_node : expected_nodes) {
        checkDeletedNode(backend, expected_node.getLabelRule());
    }
}

vector<TreeNode> collectAllNotes()
{
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

TreeNode createNotesPageTree()
{
    auto noteNodes = collectAllNotes();
    vector<string> note_labels;
    for (auto& note : noteNodes) {
        note_labels.push_back(note.getLabelRule());
    }

    // Add the notes page tree to the backend
    TreeNode notes_page_tree = createNoContentTreeNode("notes", "Animal Notes Page", {}, 
        {1, 0, "public", maybe<string>(), maybe<string>(), maybe<string>(), maybe<int>(2)}, 
        note_labels, maybe<string>(), maybe<string>());
    return notes_page_tree;
}

void addNotesPageTree(Backend &backend)
{
    backend.upsertNode({createNotesPageTree()});
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

void testBackendLogically(Backend &backend, string label_prefix = "")
{
    cout << "Backend upsertNode utilized." << endl;
    checkMultipleGetNode(backend, prefixNodeLabels(label_prefix, createLionNodes()));
    checkMultipleGetNode(backend, prefixNodeLabels(label_prefix, createElephantNodes()));
    checkMultipleGetNode(backend, prefixNodeLabels(label_prefix, createParrotNodes()));
    cout << "Backend getNode test passed." << endl;

    // Upsert node is being implicitly tested in the addAnimalsToBackend function combined with the checkMultipleGetNode function.

    // Check the page tree
    auto notes_pages = backend.getPageTree(label_prefix + "notes");
    if (notes_pages != prefixNodeLabels(label_prefix, collectAllNotes())) {
        cerr << "Page tree mismatch" << endl;
        throw runtime_error("Page tree mismatch");
    } else {
        cout << "Page tree test passed." << endl;
    }

    // Test the getFullTree function
    {
        MemoryTree temp_tree;
        SimpleBackend temp_backend(temp_tree);
        temp_backend.upsertNode(backend.getFullTree());
        checkMultipleGetNode(temp_backend, prefixNodeLabels(label_prefix, createElephantNodes()));
        checkMultipleGetNode(temp_backend, prefixNodeLabels(label_prefix, createLionNodes()));
        checkMultipleGetNode(temp_backend, prefixNodeLabels(label_prefix, createParrotNodes()));
        auto notes_pages = temp_backend.getPageTree(label_prefix + "notes");
        if (notes_pages != prefixNodeLabels(label_prefix, collectAllNotes())) {
            cerr << "Page tree mismatch" << endl;
            throw runtime_error("Page tree mismatch");
        } else {
            cout << "Page tree test passed." << endl;
        }
        cout << "getFullTree test passed." << endl;
    }
    
    // Delete the elephant node, and the elephant node and dossiers should be deleted, and the lion and parrot nodes should still be there
    backend.deleteNode(label_prefix + "elephant");
    checkMultipleDeletedNode(backend, prefixNodeLabels(label_prefix, createElephantNodes()));
    checkMultipleGetNode(backend, prefixNodeLabels(label_prefix, createLionNodes()));
    checkMultipleGetNode(backend, prefixNodeLabels(label_prefix, createParrotNodes()));
    cout << "Backend deleteNode test passed." << endl;

    // TODO: Not going to test queryNodes or relativeQueryNodes yet, because the feature has not been designed yet.
    // I also don't yet know what features it needs.

    // Test notifications
    // Register a listener for the lion node
    bool lion_node_created = false;
    bool lion_node_deleted = false;
    string lion_label = label_prefix + "lion";
    backend.registerNodeListener("lion_listener", lion_label, false, [lion_label, &lion_node_created, &lion_node_deleted](Backend& backend, const string listener_name, const fplus::maybe<TreeNode> node) {
        cout << "Listener " << listener_name << " notified for node: " << node.get_with_default(TreeNode()).getLabelRule() << endl;
        if (node.is_just()) {
            auto found_node = node.get_with_default(TreeNode());
            if (found_node.getLabelRule() == lion_label) {
                lion_node_created = true;
                cout << "Lion node created: " << found_node.getLabelRule() << endl;
            } else {
                cerr << "Unexpected node: " << found_node.getLabelRule() << endl;
            }
        } else {
            lion_node_deleted = true;
        }
    });
    // Upsert the lion node again to trigger the listener
    backend.upsertNode(prefixNodeLabels(label_prefix, createLionNodes()));
    backend.processNotification();
    // Check that the listener was notified
    if (!lion_node_created) {
        cerr << "Lion node listener not notified on upsert" << endl;
        throw runtime_error("Lion node listener not notified on upsert");
    } else {
        cout << "Lion node listener notified on upsert" << endl;
    }
    // Delete the lion node to trigger the listener
    backend.deleteNode(lion_label);
    backend.processNotification();
    // Check that the listener was notified
    if (!lion_node_deleted) {
        cerr << "Lion node listener not notified on delete" << endl;
        throw runtime_error("Lion node listener not notified on delete");
    } else {
        cout << "Lion node listener notified on delete" << endl;
    }
    // Deregister the listener
    backend.deregisterNodeListener("lion_listener", lion_label);
    // Check that the listener is no longer notified
    lion_node_created = false;
    lion_node_deleted = false;
    backend.upsertNode(prefixNodeLabels(label_prefix, createLionNodes()));
    backend.processNotification();
    // Check that the listener was not notified
    if (lion_node_created) {
        cerr << "Lion node listener notified on upsert after deregistering" << endl;
        throw runtime_error("Lion node listener notified on upsert after deregistering");
    } else {
        cout << "Lion node listener not notified on upsert after deregistering" << endl;
    }
    backend.deleteNode(lion_label);
    backend.processNotification();
    // Check that the listener was not notified
    if (lion_node_deleted) {
        cerr << "Lion node listener notified on delete after deregistering" << endl;
        throw runtime_error("Lion node listener notified on delete after deregistering");
    } else {
        cout << "Lion node listener not notified on delete after deregistering" << endl;
    }
    cout << "Logical tests passed." << endl;
}


void testSimpleBackend() {
    MemoryTree memory_tree;
    SimpleBackend simple_backend(memory_tree);
    addAnimalsToBackend(simple_backend);
    addNotesPageTree(simple_backend);
    testBackendLogically(simple_backend);
    cout << "SimpleBackend test passed." << endl;
}

void testThreadsafeBackend() {
    MemoryTree memory_tree;
    SimpleBackend simple_backend(memory_tree);
    ThreadsafeBackend threadsafe_backend(simple_backend);
    addAnimalsToBackend(threadsafe_backend);
    addNotesPageTree(threadsafe_backend);
    testBackendLogically(threadsafe_backend);
    cout << "ThreadsafeBackend test passed." << endl;
}

void testTransactionalBackend() {
    MemoryTree memory_tree;
    SimpleBackend simple_backend(memory_tree);
    TransactionalBackend transactional_backend(simple_backend);
    addAnimalsToBackend(transactional_backend);
    addNotesPageTree(transactional_backend);
    testBackendLogically(transactional_backend);
    cout << "TransactionalBackend test passed." << endl;
}

void testMemoryTreeIO() {
    MemoryTree memory_tree;
    SimpleBackend simple_backend(memory_tree);
    addAnimalsToBackend(simple_backend);
    addNotesPageTree(simple_backend);
    // Test the stream write operator << and read operator >>
    std::ostringstream oss;
    oss << memory_tree;
    string output = oss.str();
    std::istringstream iss(output);
    MemoryTree loaded_tree;
    iss >> loaded_tree;
    if (memory_tree != loaded_tree) {
        cerr << "MemoryTree IO test failed." << endl;
        throw runtime_error("MemoryTree IO test failed.");
    } else {
        cout << "MemoryTree IO test passed." << endl;
    }
}

void testCompositeBackend() {
    {
        MemoryTree memory_tree;
        SimpleBackend simple_backend(memory_tree);
        CompositeBackend composite_backend(simple_backend);
        addAnimalsToBackend(composite_backend);
        addNotesPageTree(composite_backend);
        testBackendLogically(composite_backend);
        cout << "CompositeBackend basic test passed." << endl;
    }
    {
        MemoryTree memory_tree;
        SimpleBackend simple_backend(memory_tree);
        CompositeBackend composite_backend(simple_backend);
        MemoryTree zoo_memory_tree;
        SimpleBackend zoo_backend(zoo_memory_tree);
        composite_backend.mountBackend("zoo", zoo_backend);
        MemoryTree museum_memory_tree;
        SimpleBackend museum_backend(museum_memory_tree);
        composite_backend.mountBackend("museum", museum_backend);
        addAnimalsToBackend(zoo_backend);
        addNotesPageTree(zoo_backend);
        addAnimalsToBackend(museum_backend);
        addNotesPageTree(museum_backend);
        testBackendLogically(composite_backend, "zoo/");
        testBackendLogically(composite_backend, "museum/");
        cout << "CompositeBackend mountBackend test passed." << endl;
    }
}

int main() {
    testSimpleBackend();
    testThreadsafeBackend();
    testTransactionalBackend();
    testMemoryTreeIO();
    testCompositeBackend();
    return 0;
}