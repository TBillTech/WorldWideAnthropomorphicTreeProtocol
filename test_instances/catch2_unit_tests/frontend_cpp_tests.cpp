
#include <catch2/catch_test_macros.hpp>
#include "simple_backend.h"
#include "backend_testbed.h"
#include "cloning_mediator.h"
#include "yaml_mediator.h"
#include "file_backend.h"
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>

TEST_CASE("Cloning Mediator copy test", "[CloningMediator]") {
    auto memory_tree_A = make_shared<MemoryTree>();
    SimpleBackend simple_backend_A(memory_tree_A);
    auto memory_tree_B = make_shared<MemoryTree>();
    SimpleBackend simple_backend_B(memory_tree_B);
    CloningMediator cloning_mediator("cloning_mediator", simple_backend_A, simple_backend_B, false);
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
    auto memory_tree_A = make_shared<MemoryTree>();
    SimpleBackend simple_backend_A(memory_tree_A);
    auto memory_tree_B = make_shared<MemoryTree>();
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
    CloningMediator cloning_mediator("cloning_mediator", simple_backend_A, simple_backend_B, true);
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
    auto memory_tree = make_shared<MemoryTree>();
    SimpleBackend nodeful_backend(memory_tree);
    auto yaml_memory_tree = make_shared<MemoryTree>();
    SimpleBackend yaml_backend(yaml_memory_tree);
    auto other_memory_tree = make_shared<MemoryTree>();
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
        YAMLMediator yaml_mediator("yaml_mediator", nodeful_backend, yaml_backend, specifier, false);
    }
    {
        YAMLMediator yaml_mediator("yaml_mediator", other_backend, yaml_backend, specifier, true);
    }
    {
        BackendTestbed tester(other_backend);
        tester.testBackendLogically();
    }
}

TEST_CASE("YAMLMediator notification test", "[YAMLMediator][notification]") {
    auto memory_tree = make_shared<MemoryTree>();
    SimpleBackend nodeful_backend(memory_tree);
    auto yaml_memory_tree = make_shared<MemoryTree>();
    SimpleBackend yaml_backend(yaml_memory_tree);
    auto other_memory_tree = make_shared<MemoryTree>();
    SimpleBackend other_backend(other_memory_tree);
    {
        BackendTestbed tester(nodeful_backend);
        tester.addAnimalsToBackend();
        tester.addNotesPageTree();
    }
    string label_rule = "universe/city/newyork";
    string property_name = "zoo";
    string property_type = "yaml";
    PropertySpecifier specifier(label_rule, property_name, property_type);
    {
        YAMLMediator nodeful_mediator("nodeful_mediator", nodeful_backend, yaml_backend, specifier, false);
        BackendTestbed tester(nodeful_backend);
        YAMLMediator other_mediator("other_mediator", other_backend, yaml_backend, specifier, true);
        tester.testPeerNotification(other_backend, 100);
    }
}

TEST_CASE("YAMLMediator SimpleBackend-FileBackend integration test", "[YAMLMediator][integration]") {
    size_t inotify_delay = 40; // milliseconds
    // Setup sandbox path and clean up any existing universe/city directory
    std::string sandbox_path = "/home/tom/sandbox/";
    std::string city_path = sandbox_path + "universe/city";
    std::string zoo_file_path = sandbox_path + "universe/city/newyork.0.zoo.yaml";
    
    // Recursively delete universe/city directory if it exists
    if (std::filesystem::exists(city_path)) {
        std::filesystem::remove_all(city_path);
    }
    
    // Require sandbox directory exists
    REQUIRE(std::filesystem::exists(sandbox_path));
    
    // Create SimpleBackend with test data
    auto memory_tree = make_shared<MemoryTree>();
    SimpleBackend simple_backend(memory_tree);
    {
        BackendTestbed tester(simple_backend);
        tester.addAnimalsToBackend();
        tester.addNotesPageTree();
    }
    
    // Create FileBackend for YAML storage
    FileBackend file_backend(sandbox_path);
    
    // Setup YAMLMediator with SimpleBackend->FileBackend (initialize_from_yaml = false)
    string label_rule = "universe/city/newyork";
    string property_name = "zoo";
    string property_type = "yaml";
    PropertySpecifier specifier(label_rule, property_name, property_type);
    
    YAMLMediator yaml_mediator("yaml_mediator", simple_backend, file_backend, specifier, false);
    
    // Process any pending notifications to ensure initial sync
    std::this_thread::sleep_for(std::chrono::milliseconds(inotify_delay));
    simple_backend.processNotifications();
    file_backend.processNotifications();
    
    // Verify that the YAML file was created and contains data from SimpleBackend
    REQUIRE(std::filesystem::exists(zoo_file_path));
    
    std::ifstream yaml_file(zoo_file_path);
    std::string yaml_content((std::istreambuf_iterator<char>(yaml_file)),
                            std::istreambuf_iterator<char>());
    yaml_file.close();
    
    // Verify the YAML contains some expected data (lion should be in there)
    REQUIRE(yaml_content.find("lion") != std::string::npos);
    
    // Test 1: Modify SimpleBackend data and verify it updates the YAML file
    auto lion_node_maybe = simple_backend.getNode("lion");
    REQUIRE(lion_node_maybe.is_just());
    
    auto lion_node = lion_node_maybe.unsafe_get_just();
    lion_node.setDescription("A magnificent predator of the African savanna");
    ++lion_node; // Increment version
    simple_backend.upsertNode({lion_node});
    
    // Process notifications and wait for file system sync
    std::this_thread::sleep_for(std::chrono::milliseconds(inotify_delay));
    simple_backend.processNotifications();
    file_backend.processNotifications();
    
    // Verify the YAML file was updated
    std::ifstream updated_yaml_file(zoo_file_path);
    std::string updated_yaml_content((std::istreambuf_iterator<char>(updated_yaml_file)),
                                    std::istreambuf_iterator<char>());
    updated_yaml_file.close();
    
    REQUIRE(updated_yaml_content.find("A magnificent predator of the African savanna") != std::string::npos);
    
    // Test 2: Manually modify the YAML file and verify it updates SimpleBackend
    // First, load and parse the current YAML
    YAML::Node yaml_doc = YAML::Load(updated_yaml_content);
    
    // Find and modify the elephant description in the YAML
    if (yaml_doc["elephant"] && yaml_doc["elephant"]["description"]) {
        yaml_doc["elephant"]["description"] = "A gentle giant with incredible memory";
    }
    
    // Write the modified YAML back to the file
    std::ofstream out_yaml_file(zoo_file_path);
    out_yaml_file << yaml_doc;
    out_yaml_file.close();
    
    // Process file system notifications and wait for sync
    std::this_thread::sleep_for(std::chrono::milliseconds(inotify_delay));
    file_backend.processNotifications();
    simple_backend.processNotifications();
    
    // Verify the SimpleBackend was updated
    auto elephant_node_maybe = simple_backend.getNode("elephant");
    REQUIRE(elephant_node_maybe.is_just());
    
    auto elephant_node = elephant_node_maybe.unsafe_get_just();
    REQUIRE(elephant_node.getDescription() == "A gentle giant with incredible memory");
}
    
