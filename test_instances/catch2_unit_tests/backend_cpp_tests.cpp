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
    // set to pool size for the type UDPChunk to 4 GB
    memory_pool.setPoolSize<UDPChunk>(static_cast<uint64_t>(4) * 1024 * 1024 * 1024 / UDPChunk::chunk_size);

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

TEST_CASE("MemoryTree IO test", "[MemoryTree][IO]") {
    MemoryTree memory_tree;
    SimpleBackend simple_backend(memory_tree);
    BackendTestbed tester(simple_backend);
    tester.addAnimalsToBackend();
    tester.addNotesPageTree();
    std::ostringstream oss;
    oss << memory_tree;
    string output = oss.str();
    std::istringstream iss(output);
    MemoryTree loaded_tree;
    iss >> loaded_tree;
    REQUIRE(memory_tree == loaded_tree);
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

TEST_CASE("SimpleBackend stress test", "[SimpleBackend][stress]") {
    MemoryTree memory_tree;
    SimpleBackend simple_backend(memory_tree);
    BackendTestbed tester(simple_backend);
    tester.stressTestConstructions(1000); // Stress test with 1000 constructions
}