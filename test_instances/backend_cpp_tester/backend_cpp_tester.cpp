#include "simple_backend.h"
#include "threadsafe_backend.h"
#include "transactional_backend.h"
#include "composite_backend.h"
#include "backend_tester.h"
#include <iostream>
#include <thread>
#include <vector>

using namespace std;
using namespace fplus;

void testSimpleBackend() {
    MemoryTree memory_tree;
    SimpleBackend simple_backend(memory_tree);
    BackendTester tester(simple_backend);
    tester.addAnimalsToBackend();
    tester.addNotesPageTree();
    tester.testBackendLogically();
    cout << "SimpleBackend test passed." << endl;
}

void testThreadsafeBackend() {
    MemoryTree memory_tree;
    SimpleBackend simple_backend(memory_tree);
    ThreadsafeBackend threadsafe_backend(simple_backend);
    BackendTester tester(threadsafe_backend);
    tester.addAnimalsToBackend();
    tester.addNotesPageTree();
    tester.testBackendLogically();
    cout << "ThreadsafeBackend test passed." << endl;
}

void testTransactionalBackend() {
    MemoryTree memory_tree;
    SimpleBackend simple_backend(memory_tree);
    TransactionalBackend transactional_backend(simple_backend);
    BackendTester tester(transactional_backend);
    tester.addAnimalsToBackend();
    tester.addNotesPageTree();
    tester.testBackendLogically();
    cout << "TransactionalBackend test passed." << endl;
}

void testMemoryTreeIO() {
    MemoryTree memory_tree;
    SimpleBackend simple_backend(memory_tree);
    BackendTester tester(simple_backend);
    tester.addAnimalsToBackend();
    tester.addNotesPageTree();
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
        BackendTester tester(composite_backend);
        tester.addAnimalsToBackend();
        tester.addNotesPageTree();
        tester.testBackendLogically();
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
        BackendTester zoo_tester(zoo_backend);
        zoo_tester.addAnimalsToBackend();
        zoo_tester.addNotesPageTree();
        BackendTester museum_tester(museum_backend);
        museum_tester.addAnimalsToBackend();
        museum_tester.addNotesPageTree();
        BackendTester tester(composite_backend);
        tester.testBackendLogically("zoo/");
        tester.testBackendLogically("museum/");
        cout << "CompositeBackend mountBackend test passed." << endl;
    }
}

int main() {
    // set to pool size for the type UDPChunk to 4 GB
    memory_pool.setPoolSize<UDPChunk>(static_cast<uint64_t>(4) * 1024 * 1024 * 1024 / UDPChunk::chunk_size);

    testSimpleBackend();
    testThreadsafeBackend();
    testTransactionalBackend();
    testMemoryTreeIO();
    testCompositeBackend();
    cout << "All tests passed." << endl;
    return 0;
}