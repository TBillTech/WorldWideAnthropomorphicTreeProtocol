#pragma once
#include "simple_backend.h"
#include "threadsafe_backend.h"
#include "transactional_backend.h"
#include "composite_backend.h"
#include <vector>
#include <string>
#include <iostream>

using namespace std;
using namespace fplus;

TreeNode createNoContentTreeNode(string label_rule, string description, vector<string> literal_types, 
    TreeNodeVersion version, vector<string> child_names, 
    maybe<string> query_how_to, maybe<string> qa_sequence);

TreeNode createAnimalNode(string animal, string description, vector<string> literal_types, 
    TreeNodeVersion version, vector<string> child_names, 
    vector<pair<int, string>> contents, string query_how_to, string qa_sequence);

vector<TreeNode> createAnimalDossiers(TreeNode &animal_node);
vector<TreeNode> createLionNodes();
vector<TreeNode> createElephantNodes();
vector<TreeNode> createParrotNodes();
void checkGetNode(Backend const &backend, const string& label_rule, TreeNode const &expected_node);
void checkMultipleGetNode(Backend const &backend, const vector<TreeNode>& expected_nodes);
void checkDeletedNode(Backend const &backend, const string& label_rule);
void checkMultipleDeletedNode(Backend const &backend, const vector<TreeNode>& expected_nodes);
vector<TreeNode> collectAllNotes();
TreeNode createNotesPageTree();
vector<TreeNode> prefixNodeLabels(string label_prefix, vector<TreeNode> nodes);

void disableCatch2();

class BackendTestbed {
public:
    Backend& backend_;
    BackendTestbed(Backend& backend, bool should_test_notifications = true);
    void addAnimalsToBackend();
    void addNotesPageTree();
    void testBackendLogically(string label_prefix = "");

private:
    bool should_test_notifications_;
    bool useCatch2_ = true;
};
