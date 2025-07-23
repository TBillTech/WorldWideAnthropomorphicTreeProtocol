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

1. **WWATPService**
   - The WWATPService class is a class designed to set up various backends, frontends, and import plugins, all to initialize a functional WWATP based capability.
   - The WWATPService class takes a given backend and node config label (by default "config", assumed for the rest of this description), and performs the above task.
   - The WWATPService is in fact a kind of frontend, since it takes a backend when constructed.
   - The WWATPService class provides access to all constructed backends via the name of the backend in the config. (This allows the server or other application to access backends by name).
   - The config/ contains a child node called backends which has a child for each backend defined in the server process.  The properties of each child node of the backends will define how that backend is composed.  These properties should mirror the constructor arguments of the backend_cpp backends, _including_ the http3_client backend.  This allows for the construction of service oriented architectures.  Note that instead of providing an actual Backend reference for a constructor argument, it will be the name of another backend child node.
   - In order to properly handle the above, the WWATPService will have to construct a topological sort to make sure all backends are constructed in the proper order and relationship.
   - The config/ also contains a child node called frontends which has a child for each frontend defined in the server process.  The properties of each child node of the frontends will define how that frontend is composed.  These properties should mirror the constructor arguments of the frontends.  The following list will be modified in the future as more frontends are constructed in the project:
       - Frontends supported in the server config: http3_server, cloning_mediator, yaml_mediator, plugin_mediator, plugin_model
   - The following breaks down how the http3_server config node is interpreted by the WWATPService.

2. **The Server Config Node**
   - Like the other Frontends, the http3_server config node will be stored in the Frontends.  In this description, the example is given for a server config node called "server". One must be careful if multiple http3_server frontends are added to the WWATPService, that they have differing port numbers, but it is allowed if necessary.  
   - The server will have a special file/property called .0.config.yaml:
       - server.node
       - server/
       - server.0.config.yaml
       - other files ...
   - The .0.config.yaml file will contain a yaml map with the following elements, shown here with example values:
       - name: "some_server_name"
       - port: 443
       - anthing else needed to instantiate the server.

3. **Server Node Initialization**
   - The server assumes the given argument node for config (here called "server") is a WWATP node.
   - The server node is used to define all the static assets as well as the WWATP routes.
   - In the following, whatever the node's actual name is would replace "server".
   - The server checks the existence and proper formatting of the server node in the directory that the server starts up in.  For example this will be:
       - server.node
       - server/
       - server.0.config.yaml
       - server.1.index.html
   - The server static files that have no url path will be stored as server properties. Here are some examples:
       - server.2.styles.css
       - server.3.http3_client.js
       - server.4.domlink.js
   - The server static files that do have url paths will be stored in server child node properties, for example the plotly/plotly.js might be stored like this:
       - server/plotly.node
       - server/plotly/
       - server/plotly.0.plotly.js
   - The server routes will be defined by nodes and properties under wwatp (which does preclude static assets having a path of wwatp). For example, in the following, the default tree will be at https://hostname:port/wwatp, and the deeper api tree would be at https://hostname:port/wwatp/a_deeper_api.  The backend strings will match up to the names of other backends in the WWATPService.
       - server/wwatp.node
       - server/wwatp/
       - server/wwatp.0.backend.string
       - server/wwatp/a_deeper_api.node
       - server/wwatp/a_deeper_api
       - server/wwatp/a_deeper_api.0.backend.string



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