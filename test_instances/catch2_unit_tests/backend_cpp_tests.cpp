#include <catch2/catch_test_macros.hpp>
#include "simple_backend.h"
#include "threadsafe_backend.h"
#include "transactional_backend.h"
#include "composite_backend.h"
#include "backend_testbed.h"
#include <sstream>

using namespace std;
using namespace fplus;

TEST_CASE("SimpleBackend logical test", "[SimpleBackend]") {
    MemoryTree memory_tree;
    SimpleBackend simple_backend(memory_tree);
    BackendTestbed tester(simple_backend);
    tester.addAnimalsToBackend();
    tester.addNotesPageTree();
    tester.testBackendLogically();
}

TEST_CASE("ThreadsafeBackend logical test", "[ThreadsafeBackend]") {
    MemoryTree memory_tree;
    SimpleBackend simple_backend(memory_tree);
    ThreadsafeBackend threadsafe_backend(simple_backend);
    BackendTestbed tester(threadsafe_backend);
    tester.addAnimalsToBackend();
    tester.addNotesPageTree();
    tester.testBackendLogically();
}

TEST_CASE("TransactionalBackend logical test", "[TransactionalBackend]") {
    MemoryTree memory_tree;
    SimpleBackend simple_backend(memory_tree);
    TransactionalBackend transactional_backend(simple_backend);
    BackendTestbed tester(transactional_backend);
    tester.addAnimalsToBackend();
    tester.addNotesPageTree();
    tester.testBackendLogically();
}

TEST_CASE("A few I/O tests", "[Version][IO]") {
    TreeNodeVersion version1;
    version1.version_number = 1;
    version1.max_version_sequence = 10;
    version1.policy = "default";
    version1.authorial_proof = maybe<string>("proof");
    version1.authors = maybe<string>("author1,author2");
    version1.readers = maybe<string>("reader1,reader2");
    version1.collision_depth = maybe<int>(100);
    TreeNodeVersion version2;
    version2.version_number = 2;
    version2.max_version_sequence = 10;
    version2.policy = "default";
    version2.authorial_proof = maybe<string>("proof");
    version2.authors = maybe<string>("author1,author2");
    version2.readers = maybe<string>("reader1,reader2,reader3");
    version2.collision_depth = maybe<int>(100);
    TreeNodeVersion minimalVersion;
    minimalVersion.version_number = 1;
    minimalVersion.max_version_sequence = 1;
    // A lambda function to test the version structure serialization and deserialization
    auto testVersionIO = [](const TreeNodeVersion& version) {
        {
            std::ostringstream oss;
            oss << version;
            string output = oss.str();
            std::istringstream iss(output);
            TreeNodeVersion loaded_version;
            iss >> loaded_version;
            REQUIRE(version == loaded_version);
        }
    };
    testVersionIO(version1);
    testVersionIO(version2);
    testVersionIO(minimalVersion);
    {
        auto simpleAnimal = createAnimalNode("Sponge", "Bottom Feeder", {}, 
        {1, 1}, {}, {}, "", "");
        std::ostringstream oss;
        oss << simpleAnimal;
        string output = oss.str();
        std::istringstream iss(output);
        TreeNode loaded_simple_animal;
        iss >> loaded_simple_animal;
        REQUIRE(simpleAnimal == loaded_simple_animal);
    }
}

TEST_CASE("MemoryTree IO test", "[MemoryTree][IO]") {
    MemoryTree memory_tree;
    SimpleBackend simple_backend(memory_tree);
    BackendTestbed tester(simple_backend);
    tester.addAnimalsToBackend();
    tester.addNotesPageTree();
    {
        std::ostringstream oss;
        oss << memory_tree;
        string output = oss.str();
        std::istringstream iss(output);
        MemoryTree loaded_tree;
        iss >> loaded_tree;
        REQUIRE(memory_tree == loaded_tree);
    }
    auto mlion_node = memory_tree.getNode("lion");
    REQUIRE(mlion_node.is_just());
    auto lion_node = mlion_node.get_with_default(TreeNode());
    {
        std::ostringstream oss;
        oss << hide_contents << lion_node;
        string output = oss.str();
        std::istringstream iss(output);
        TreeNode loaded_lion_node;
        iss >> loaded_lion_node;
        lion_node.setContents(shared_span<>(global_no_chunk_header, false)); // Clear contents for comparison
        REQUIRE(lion_node == loaded_lion_node);    
    }
}

TEST_CASE("CompositeBackend basic test", "[CompositeBackend]") {
    MemoryTree memory_tree;
    SimpleBackend simple_backend(memory_tree);
    CompositeBackend composite_backend(simple_backend);
    BackendTestbed tester(composite_backend);
    tester.addAnimalsToBackend();
    tester.addNotesPageTree();
    tester.testBackendLogically();
}

TEST_CASE("CompositeBackend mountBackend test", "[CompositeBackend][mountBackend]") {
    MemoryTree memory_tree;
    SimpleBackend simple_backend(memory_tree);
    CompositeBackend composite_backend(simple_backend);
    MemoryTree zoo_memory_tree;
    SimpleBackend zoo_backend(zoo_memory_tree);
    composite_backend.mountBackend("zoo", zoo_backend);
    MemoryTree museum_memory_tree;
    SimpleBackend museum_backend(museum_memory_tree);
    composite_backend.mountBackend("museum", museum_backend);
    BackendTestbed zoo_tester(zoo_backend);
    zoo_tester.addAnimalsToBackend();
    zoo_tester.addNotesPageTree();
    BackendTestbed museum_tester(museum_backend);
    museum_tester.addAnimalsToBackend();
    museum_tester.addNotesPageTree();
    BackendTestbed tester(composite_backend);
    tester.testBackendLogically("zoo/");
    tester.testBackendLogically("museum/");
}
