# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

WWATP (World Wide Anthropomorphic Tree Protocol) is an agent communication layer built on top of QUIC/HTTP3. It implements a tree-based data structure where each endpoint maintains a growing tree that can be modified through branches, leaves, and mutations. The protocol is designed to be LLM-friendly with anthropomorphic capabilities where nodes can reference personality prompts for agents. See Readme.md for more information.

## Build System & Commands

The project uses CMake as the primary build system:

**Core build commands:**
```bash
# Build the project
cmake -B build
make -C build

# Run main test executable
./build/library_tester

# Run unit tests with Catch2
./build/catch2_unit_tests

# Run memory pool tests
./build/memory_pool_tester

# Run HTTP3 client backend tests
./build/http3_client_backend_tester
```

**Testing:**
- Main test: `ctest` or `./build/library_tester` 
- Unit tests: `./build/catch2_unit_tests` (uses Catch2 framework)
- Backend tests: `./build/http3_client_backend_tester`
- Memory tests: `./build/memory_pool_tester`

## Architecture Overview

The codebase follows a layered frontend/backend architecture:

### Core Components

**Backend Layer** (`common/backend_cpp/`):
- `Backend` interface - Pure virtual base class defining tree operations (getNode, upsertNode, deleteNode, queryNodes, etc.)
- `TreeNode` - Core data structure representing tree nodes with versioning, properties, and metadata
- Memory backends: SimpleBackend, TransactionalBackend, ThreadsafeBackend
- Structure backends: CompositeBackend, RedirectedBackend  
- HTTP3ClientBackend for network communication
- FileBackend for file system operations

**Frontend Layer** (`common/frontend/`):
- HTTP3Server frontend for network services
- Mediator frontends: CloningMediator, YAMLMediator for data synchronization

**Transport Layer** (`common/transport/`):
- QUIC/HTTP3 communication using ngtcp2 and nghttp3
- TLS support with BoringSSL
- Memory pool management for efficient data handling

### Key Design Patterns

1. **Backend Composition**: Backends can be layered (e.g., ThreadsafeBackend wrapping SimpleBackend)
2. **Frontend-Backend Pairing**: Each frontend usually connects to exactly one backend (mediators are two); backends can serve multiple frontends
3. **Tree Versioning**: Nodes have version numbers with policies (Reader-Writer, Token Ring, Collision Rollback)
4. **Transaction Support**: Atomic operations across multiple nodes with rollback capabilities

## Dependencies

**External Libraries:**
- ngtcp2/nghttp3: QUIC/HTTP3 implementation
- BoringSSL: Cryptographic operations
- yaml-cpp: YAML serialization support
- FunctionalPlus: Functional programming utilities
- Catch2: Unit testing framework (fetched automatically)
- Boost: System utilities

**Library Management:**
- Third-party libraries are in `/libraries/` and built via ExternalProject_Add or
- BoringSSL, nghttp3, ngtcp2 are built from specific git commits for stability

## Development Notes

**Code Style:**
- C++20 standard required
- Uses functional programming patterns via FunctionalPlus
- RAII patterns with shared_span<> for memory management
- Template-based property system in TreeNode

**Key Files to Understand:**
- `common/backend_cpp/interface/backend.h:22` - Core Backend interface
- `common/backend_cpp/interface/tree_node.h:145` - TreeNode class definition
- `common/transport/include/` - Transport layer headers
- `test_instances/catch2_unit_tests/` - Unit test examples

**Testing Strategy:**
- Unit tests cover individual components (TreeNode, SharedChunk, HTTP3 messages)
- Integration tests via backend testbed framework
- Memory pool and HTTP3 client backend have dedicated test executables