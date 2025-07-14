#include <catch2/catch_test_macros.hpp>
#include "simple_backend.h"
#include "threadsafe_backend.h"
#include "transactional_backend.h"
#include "composite_backend.h"
#include "redirected_backend.h"
#include "backend_testbed.h"
#include "file_backend.h"
#include <sstream>
#include <filesystem>

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

TEST_CASE("RedirectedBackend test", "[RedirectedBackend]") {
    MemoryTree memory_tree;
    SimpleBackend simple_backend(memory_tree);
    MemoryTree zoo_memory_tree;
    SimpleBackend zoo_backend(zoo_memory_tree);
    CompositeBackend composite_backend(simple_backend);
    composite_backend.mountBackend("zoo", zoo_backend);
    {
        BackendTestbed tester(zoo_backend);
        tester.addAnimalsToBackend();
        tester.addNotesPageTree();
    }
    {
        RedirectedBackend redirected_backend(composite_backend, "zoo");
        BackendTestbed tester(redirected_backend);
        tester.testBackendLogically();
    }
}

TEST_CASE("SimpleBackend stress test", "[SimpleBackend][stress]") {
    MemoryTree memory_tree;
    SimpleBackend simple_backend(memory_tree);
    BackendTestbed tester(simple_backend);
    tester.stressTestConstructions(1000); // Stress test with 1000 constructions
}

TEST_CASE("FileBackend test", "[FileBackend]") {
    string base_path = "/home/tom/sandbox/file_backend_test_store";
    // First, clean up the file backend store directory if it exists
    std::filesystem::remove_all(base_path);
    std::filesystem::create_directory(base_path);

    {
        FileBackend file_backend(base_path);
        BackendTestbed tester(file_backend);
        tester.addAnimalsToBackend();
        tester.addNotesPageTree();
        tester.testBackendLogically();
        // This should ready the backend for testBackendLogically
        tester.addAnimalsToBackend();
    }
    {
        // This picks up the files from the previous test
        // and tests the logical consistency of the backend again.
        FileBackend file_backend(base_path);
        BackendTestbed tester(file_backend);
        tester.testBackendLogically();
        // This rewrites the files one more time so the human can browse them.
        tester.addAnimalsToBackend();
    }
    {
        // This is a paranoid check to see if inotify works correctly.
        // It will directly create a infotify_fd_, and a wd for the file:
        // ../test_instances/sandbox/file_backend_store/lion.node
        // Then it will write some bytes to that file, overwriting it.
        // Then it will test to see if the inotify_fd_ is notified of the change.
        auto inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (inotify_fd < 0) {
            throw std::runtime_error("Failed to initialize inotify");
        }
        int wd = inotify_add_watch(inotify_fd, (base_path + "/lion.node").c_str(), IN_MODIFY);
        if (wd < 0) {
            perror("inotify_add_watch");
            throw std::runtime_error("Failed to add inotify watch for lion.node");
        }
        // Now write some bytes to the file
        std::ofstream lion_file(base_path + "/lion.node");
        if (!lion_file) {
            throw std::runtime_error("Failed to open lion.node for writing");
        }
        lion_file << "This is a test of inotify.\n";
        lion_file.close();
        // Now read the inotify events
        char buffer[4096];
        int loops = 10000;
        int sleep_time = 500; // milliseconds
        int total = loops*sleep_time;
        bool notified = false;
        while (loops-- > 0) {
            ssize_t length = read(inotify_fd, buffer, sizeof(buffer));
            if (length < 0) {
                int err = errno;
                if (err == EAGAIN) {
                    // No events available, not an error in non-blocking mode
                } else {
                    std::cerr << "Error reading inotify events: " << strerror(err) << std::endl;
                    throw std::runtime_error("Failed to read inotify events");
                }
            }
            if (length > 0) {
                // We got some events, let's check if we got the one we expected
                size_t offset = 0;
                while (offset < length) {
                    struct inotify_event* event = reinterpret_cast<struct inotify_event*>(&buffer[offset]);
                    if (event->wd == wd && (event->mask & IN_MODIFY)) {
                        //std::cout << "Inotify notified of modification to lion.node" << std::endl;
                        notified = true;
                        break; // We found the event we were looking for
                    }
                    offset += sizeof(struct inotify_event) + event->len;
                }
            }
            if (notified) {
                break; // We got the notification we were looking for
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time));
        }
        inotify_rm_watch(inotify_fd, wd);
        close(inotify_fd);
        if(!notified) {
            cerr << "Could not get inotify notification for lion.node modification even after " << total << " milliseconds" << endl << flush;
        }
        REQUIRE(notified); // We should have been notified of the modification
    }
    {
        FileBackend to_be_notified_backend(base_path);
        BackendTestbed tester(to_be_notified_backend);
        FileBackend to_be_modified_backend(base_path);
        tester.testPeerNotification(to_be_modified_backend, 40);
    }
    {
        FileBackend to_be_notified_backend(base_path);
        BackendTestbed tester(to_be_notified_backend);
        FileBackend to_be_modified_backend(base_path);
        tester.testPeerNotification(to_be_modified_backend, 40, "zoo/Seattle/");
    }
    {
        FileBackend file_backend(base_path);
        BackendTestbed tester(file_backend);
        // This rewrites the files one more time so the human can browse them.
        tester.addAnimalsToBackend();
    }
}

