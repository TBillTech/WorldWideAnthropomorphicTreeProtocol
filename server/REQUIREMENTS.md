# WWATP Server Requirements

## Current Status

### Current Architecture

```
┌─────────────────┐    ┌──────────────────┐    ┌─────────────────┐
│   QuicListener  │───▶│   HTTP3Server    │───▶│  WWATPService  │
│  (Network I/O)  │    │   (Frontend)     │    │ (Tree Storage)  │
└─────────────────┘    └──────────────────┘    └─────────────────┘
```

## Requirements

### High Priority Features

1. **Current Working Directory startup**
   - The server assumes the the current working directory that it starts up in contains the server WWATP node
   - OR the first argument to the server executable is a directory containing the server WWATP node.
   - By default, the server looks for a node called server, but this name may be overridden on the command line.
   - The server checks the existence and proper formatting of the server node in the directory that the server starts up in.  By default this will be:
       - server.node
       - server/
       - server.0.config.yaml
   - The server static files that have no url path will be stored as server properties, here are some examples given the default server node:
       - server.1.index.html
       - server.2.styles.css
       - server.3.http3_client.js
       - server.4.domlink.js
   - The server static files that do have url paths will be stored and server child node properties, for example the plotly/plotly.js might be stored like this:
       - server/plotly.node
       - server/plotly/
       - server/plotly.0.plotly.js
   - Once the server confirms basic consistency and server node existence on disk, the config.yaml and the static files will be implemented by simply using the FileBackend.

2. **server.0.config.yaml**
   - The server.0.config.yaml is a WWATP property that can be used with the YAMLMediator to un-pickle the server configuration.  The yaml will be unpacked into a tree node (a memory tree with a config node for the properties).   
   - Here is the current description of the config settings:
       - config.node
       - config/
       - config.0.name.string: The name of the server
       - config.1.port.string: The port the server listens on
   - The server architecture will be constructed from the config node by leveraging the WWATPService.  This implies that as well as carrying any server config properties, the config node will also be an initialization object for the WWATPServer. The WWATPService is a class that accepts a configuration node (with optional children) and constructs, sets up, and imports the needed backends and frontends. See the next in the requirements list.
   - The config/ contains a child node called wwatp which has a child for each end point the server serves.  A client requesting the endpoint will notionally prepend http://theserver:port/wwatp/ to the url so that, for example, http://theserver:port/wwatp/research would be the tree defined by the config/wwatp/research node.  The wwatp endpoint nodes shall define a backend name which will match one of the child nodes in config/backends/ . 

3. **WWATPService**
   - The WWATPService class is a class designed to set up various backends, frontends, and import plugins, all to initialize a functional WWATP based capability.
   - The WWATPService class takes a given backend and node config label (by default "config", assumed for the rest of this description), and performs the above task.
   - The WWATPService is in fact a kind of frontend, since it takes a backend when constructed.
   - The WWATPService class provides access to all constructed backends via the name of the backend in the config. (This allows the server or other application to access backends by name).
   - The config/ contains a child node called backends which has a child for each backend defined in the server process.  The properties of each child node of the backends will define how that backend is composed.  These properties should mirror the constructor arguments of the backend_cpp backends, _including_ the http3_client backend.  This allows for the construction of service oriented architectures.  Note that instead of providing an actual Backend reference for a constructor argument, it will be the name of another backend child node.
   - In order to properly handle the above, the WWATPService will have to construct a topological sort to make sure all backends are constructed in the proper order and relationship.
   - The config/ also contains a child node called frontends which has a child for each frontend defined in the server process.  The properties of each child node of the frontends will define how that frontend is composed.  These properties should mirror the constructor arguments of the frontends, but some of the frontends do NOT apply.  For example, the http3_server_cpp frontend makes no sense in this context.  The following list will be modified in the future as more frontends are constructed in the project:
       - Frontends NOT supported in the server config:  http3_server_cpp
       - Frontends supported in the server config: cloning_mediator, yaml_mediator, plugin_mediator, plugin_model

## Technical Requirements

### Dependencies
- C++20 compiler support
- CMake 3.10+
- Boost.Asio for I/O
- BoringSSL for cryptography
- ngtcp2/nghttp3 for QUIC/HTTP3
- yaml-cpp for configuration
- FunctionalPlus for utilities

### Build Requirements
- All features must integrate with existing CMake build system
- Must maintain compatibility with existing test suite
- Should follow existing code patterns and conventions

### Performance Requirements
- Support for concurrent client connections

## Development Guidelines

1. **Code Organization**
   - New features should be modular and testable
   - Follow existing directory structure patterns
   - Maintain separation between frontend/backend layers

2. **Testing Strategy**
   - Unit tests for new components
   - Integration tests for server functionality
   - Performance benchmarks for critical paths

3. **Documentation**
   - Update README.md for new features
   - Maintain inline code documentation
   - Update CLAUDE.md build instructions as needed

## Migration Path

The current stub implementation provides a solid foundation for incremental feature development. Each new feature can be added while maintaining backwards compatibility with the existing minimal server functionality.