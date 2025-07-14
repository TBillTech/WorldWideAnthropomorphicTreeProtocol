
#include <catch2/catch_test_macros.hpp>
#include "simple_backend.h"
#include "backend_testbed.h"
#include "cloning_mediator.h"

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