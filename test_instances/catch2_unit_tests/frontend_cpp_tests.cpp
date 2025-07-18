
#include <catch2/catch_test_macros.hpp>
#include "simple_backend.h"
#include "backend_testbed.h"
#include "cloning_mediator.h"
#include "yaml_mediator.h"

TEST_CASE("Cloning Mediator copy test", "[CloningMediator]") {
    MemoryTree memory_tree_A;
    SimpleBackend simple_backend_A(memory_tree_A);
    MemoryTree memory_tree_B;
    SimpleBackend simple_backend_B(memory_tree_B);
    CloningMediator cloning_mediator(simple_backend_A, simple_backend_B, false);
    {
        BackendTestbed tester_A(simple_backend_A);
        tester_A.addAnimalsToBackend();
        tester_A.addNotesPageTree();
    }
    {
        BackendTestbed tester_B(simple_backend_B);
        tester_B.testBackendLogically();
    }
}

TEST_CASE("Cloning Mediator versioned test", "[CloningMediator][versioned]") {
    MemoryTree memory_tree_A;
    SimpleBackend simple_backend_A(memory_tree_A);
    MemoryTree memory_tree_B;
    SimpleBackend simple_backend_B(memory_tree_B);
    {
        BackendTestbed tester_A(simple_backend_A);
        tester_A.addAnimalsToBackend();
        tester_A.addNotesPageTree();
    }
    {
        BackendTestbed tester_B(simple_backend_B);
        tester_B.addAnimalsToBackend();
        tester_B.addNotesPageTree();
    }
    CloningMediator cloning_mediator(simple_backend_A, simple_backend_B, true);
    auto prior_node = simple_backend_A.getNode("lion");
    {   // Test that updating to a new version on A shows up on B
        auto lion_node = simple_backend_A.getNode("lion");
        auto lion = lion_node.get_with_default(TreeNode());
        lion.setDescription("A king of the jungle, known for its majestic mane.");
        ++lion; // Increment the version
        simple_backend_A.upsertNode({lion});
        auto other_lion_node = simple_backend_B.getNode("lion");
        REQUIRE(other_lion_node.get_with_default(TreeNode()) == lion);
    }
    {   // Test that updating to an old version on A does not change B
        auto lion_node = prior_node.get_with_default(TreeNode());
        simple_backend_A.upsertNode({lion_node});
        auto other_lion_node = simple_backend_B.getNode("lion");
        REQUIRE(other_lion_node.get_with_default(TreeNode()) != lion_node);
    }
    {   // Test that updating to a new version on B shows up on A
        auto lion_node = simple_backend_B.getNode("lion");
        auto lion = lion_node.get_with_default(TreeNode());
        lion.setDescription("A large carnivorous feline. (Revised)");
        ++lion; // Increment the version
        simple_backend_B.upsertNode({lion});
        auto other_lion_node = simple_backend_A.getNode("lion");
        REQUIRE(other_lion_node.get_with_default(TreeNode()) == lion);
    }
}

TEST_CASE("YAMLMediator construction test", "[YAMLMediator]") {
    MemoryTree memory_tree;
    SimpleBackend nodeful_backend(memory_tree);
    MemoryTree yaml_memory_tree;
    SimpleBackend yaml_backend(yaml_memory_tree);
    MemoryTree other_memory_tree;
    SimpleBackend other_backend(other_memory_tree);
    {
        BackendTestbed tester(nodeful_backend);
        tester.addAnimalsToBackend();
        tester.addNotesPageTree();
    }
    {
        // TreeNode yaml_node = createAnimalNode(
        // "everything", 
        // "Stores YAML", 
        // {},
        // {1, 256, "public", maybe<string>(), maybe<string>(), maybe<string>(), maybe<int>(2)}, 
        // {"Dumbo", "Babar"}, 
        // {},
        // "url duh!", 
        // "Zookeeper1: Elephants are so strong.\nZookeeper2: And they have great memory!"
        // );

        //nodeful_backend.upsertNode({yaml_node});
    }
    string label_rule = "universe/yaml";
    string property_name = "zoo";
    string property_type = "yaml";
    PropertySpecifier specifier(label_rule, property_name, property_type);
    {
        YAMLMediator yaml_mediator(nodeful_backend, yaml_backend, specifier, false);
    }
    {
        YAMLMediator yaml_mediator(other_backend, yaml_backend, specifier, true);
    }
    {
        BackendTestbed tester(other_backend);
        tester.testBackendLogically();
    }
}