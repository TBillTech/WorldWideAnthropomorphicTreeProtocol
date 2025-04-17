# World Wide Anthropomorphic Tree Protocol

The World Wide Anthropomorphic Tree Protocol (WWATP) is an agent communication layer on top of QUIC. Each end-point conceptually contains a "growing" tree. In general, trees can grow in several ways:
- New branches and/or leaves.
- Pruned branches and/or leaves.
- Mutations of branches and/or leaves.

What makes this an _anthropomorphic_ tree protocol is that the description node references can be personality prompts to agents templated by the node label rule! And because queries can be agent-like string requests prompted by the Q&A of a node. Another way to look at this is to say that the literal API of clients and servers of the tree protocol is infrastructure only: the content API is implicit and based around LLM friendly two way discussions about capabilities and content descriptions.

## Client and Server instances and ecosystem

There are multiple software instances attached to the tree protocol with the following typical uses:
- **Server**: A Server instance attached to the tree protocol which efficiently stores and maintains the Tree data, and connects to multiple authentication and server peer nodes. It _may_ also communicate using inter process communication to increase bandwidth if a trusted client is running on the same CPU.  It usually has a relational database back end.
- **Authentication Node**: An instance whose job is to keep a version of the tree which exactly matches the authorization level of this node. No more, no less. This node has these sub trees:
  - Authorization branch: Not visible or queryable by clients.  Contains all the information necessary for this node to behave correctly.
  - Public branch: visible and queryable by all clients.
  - Private branch: only visible and queryable by the exact client.
- **Load Balancer Node**: From an IP standpoint, actually is the service at the IP address and port of the tree.  It's job is to redirect traffic flows, and so it possesses all the Authorization branches (and keeps nothing else) and knows which authorization instances are available to match and service them. Also has legacy tunneling from Client facing http2 websockets down to Authentication node facing QUIC connections.
- **Client**: Conceptually, connects directly to a server (usually in fact a load balancer instance), and is linked into the client software as a library. Implementations must allow the server to provide proxy sources transparently, and must allow the linked software to control how much of the tree is kept and maintained in memory. There is a javascript web browser implementation library that uses legacy http3 websockets to connect to the load balancer node.

## Tree Structure

The tree protocol has a tree structure, with node branches having any number of node children. A leaf is simply a node with no children. A branch is simply a node with children. A node has:
- **A _unique_ global label rule**: UTF8 encoded string
- **A description**: UTF8 encoded string OR a node reference
- **Optional How to query**: node reference to description of how to query and unpack nodes matching the label rule (If absent, means just the point query)
- **Optional Q&A sequence**: node reference to Question and Answer session about the node
- **A literal type**: UTF8 encoded type name, or list of type names (see below)
- **Optional policy**: node reference to versioning policy
- **A version**:
  - Sequence Number % Max Sequence, Max Sequence, UTF8 policy
  - Optional: Authorial proof (a node reference)
  - Optional: Authors: for TokenRing A node containing the authors, permissions, rules, etc.
  - Optional: Readers
  - Optional: Collision depth
- **Child names**: List of [UTF8 label templates]
- **Contents**: Length-Value bytes

## Lineage Representation

A common idiom is to represent lineage by the concatenation of labels by inserting '/' characters, and prepending with `wwatp://<addr>:<port>/` or `udp://<addr>:<port>/`, where `<addr>` is an IP address and `<port>` is the port number. Notionally, there should NOT be more than one tree protocol at any addr+port destination, because that just means there should be two sub-trees connected at the root.

## UTF8 Encoded Type Names

The UTF8 encoded type name is one of the JSON types, with the addition of Bytes and Callbacks:
- UTF8 String
- Int
- Float
- Image
- List
- Dictionary
- Bytes
- Datetime (UTF8 string coding like "DD-MM-YYYY HH:MM:SS.SSSS UTC")
- Callback
- Removed - means the node and all its children have been dropped from the tree.
- Error - means that a problem was encountered reading the stream, and the node was not correctly decoded.

Normally, branches will be labelled with List, Dictionary, or Callback, while leaves will be labelled UTF8 String, Int, Float, Image.

## Tree Encoding

The literal encoding of the full tree at the UDP layer is as a page of the total depth-first list of `<Length-Value>` node representations, for example:
{<Node uuid><Node checksum>}<Node label><Node description><Node type><Node version><Node child names><Node contents>
The Node uuid and checksum are present for udp implementations, and absent for tcp implementations. This allows for udp implementations to detect errors in the stream.

## Version Conflict Policies

Implementations should honor several common version conflict policies:
- **Reader-Writer policy**: There is one owner/reader of any given node. Only the owner is allowed to modify that node (matching the author ID), and any version of the node that has an author other than the owner is immediately rejected. Nodes without Author IDs are owned by parents. If authorial proof is provided, implementations are required to check the consistency of the signature and reject false signatures.
- **Token Ring policy**: Like the writer part of the reader-writer policy, except after each new version of the node(s), only the next Author ID in the ring has write permission. New versions that use the wrong author ID are rejected. The author ring itself is a list child of the node with reserved name "tokenring", and children author names with optional child Bytes with label "certificate". If no changes are made by an agent, then that agent should set the Author ID to itself, and leave the sequence number unchanged. The author ring also has a properties "timeout", "count" and "votes". Votes are reader-writer owned by the authors in the ring, and when the count vote has been cast, after timeout time the token will be implicitly moved to the next author in the ring. This is to prevent dead-lock.
- **Collision Rollback Policy**: All authors can freely read/write any node. Collision depth versions of nodes and all children must be kept as history until a version is final. If two messages with the same sequence ID are seen, then all agents will roll back the node to the sequence number one prior, and all "future" history will be deleted. The snapshot will have the children states at the time the rollback sequence number was first seen. Implementations should wait a random amount of time before sending the conflicting sequence change, increasing this wait time as collisions continue to occur (similar to how ethernet works).

It is recommended that only changes to the tree be sent across the protocol unless an agent specifically requests the entire tree. By definition, every callback is a pending tree substitution, and note that the Callback value has the following possible values:
- Once
- Auto

## Underlying technologies for transport layer

At present the Agents attaching to the tree will use QUIC and HTTP/3 communication.  This will permit, for example, different branches of the tree to be sent either whole or in part to multiple other agents simultaneously.

For now, the QUIC + HTTP/3 library will be nghttp3.

After further reasearch, the HTTP/3 protocol shows some difficulties. Crucially, it appears that when the server blocks due to no data ready, then the client also blocks.  This implies that the desired following flow is broken.  The client should connect to the server on a stream, then client sends messages to the server, then the server reads those messages, then the server replies to the messages, and repeat. The broken bit is that when the server "blocks" based on not having more data to send, then the client also "blocks" on sending data even if it has data to send.

Workaround: Logically disconnect RequestIdentifiers of the QuicConnector and QuicListener from QUIC streams. On client side, send a "request" which is actually a sequence of chunks with RequestIdentifiers in a batch.  Then, on server side, the new "stream" gets created, with a single "request" composed of a batch of RequestIdentified chunks, and then a single "reply" composed of a batch of RequestIdentified result chunks is sent back allowing the stream to close.  If there is enough data being sent back and forth, it might even keep the stream open.


## Backends, Frontends, and how they are composed

While the transport layer is super important, it is also abstracted away.  A frontend choice combined with a backend choice in effect creates an application.  Every frontend has exactly one backend.  Backends, on the other hand, can be composed of multiple other backends, or they may virtualize parts of the tree via agentic generation.

A special remark for supporting web applications is that http3_server_cpp has wwatp behavior only for the magic wwatp tree root.  Otherwise, index.html will provide the dom bootstrap logic which can usually make "something" out of the tree in terms of css and dom elements.  I'm expecting to create an LLM tool called a css-watcher that watches both the css file and the tree, synchronizing them, and providing the css as a node in the tree that the index.html will reference. Then mostly what is required by the js_client_lib is to apply the dom_link_javascript frontend.

Current architecture has the following details:
cpp_client_lib supports developing a tool (tools can be applications with UIs):
* frontend is direct_cpp for a tool
* backend is http3_client for a tool
js_client_lib supports developing a web application:
* frontend is direct_javascript for a web application
* backend is http3_client for a web application
The server program generally supports simple services without additional development, including services that need to restructure or remap the tree in simple ways, and services which can be backed by trees in memory (and on disk):
* frontend is http3_server_cpp for a service
* backend is http3_client, memory, or composite (which composes more composite, http3_client, and memory backends)
cpp_client_lib also supports developing custom services (for example, a custom postgres service)
* frontend is http3_server_cpp for a custom service
* backend implements interface
* If using the server program for all composition needs is too burdensome on system resources, a custom service can also use composite and other backends

To summarize what goes into the two libraries, everything from common:
cpp_client_lib: frontend; direct_cpp http3_server_cpp, backend_cpp; composite interface http3_client memory 
js_client_lib: frontend; direct_javascript dom_link_javascript, backend_js; http3_client

Here are some examples:
* A game client composed of an unreal_engine frontend using an http3_client backend_cpp.
* A game client server composed of an http3_server_cpp frontend and a postgres backend_cpp.
* A digital employee composed of:
    * A browser dom_link_javascript frontend using an http3_client backend_js.
    * A composer for browser with http3_server_cpp frontend presenting the curent task, backlog, status, and the css blob (memory backend?)
    * A css watcher tool with css_watcher frontend and http3_client backend_js informing the composer.
    * A wwatp hub http3_server_cpp frontend using a postgres backend_cpp.
    * A worker manager using direct_cpp frontend and http3_client backend_cpp, which can create worker/tool processes:
    * Multiple worker/tool processes using direct_cpp frontend and http3_client backend_cpp, additionally tools having various ways to transform tree information to additional tree information.  For example, performing algebra on equations using maxima.  Or watching a yaml file, and keeping the tree in sync with the yaml file.

Currently, the following backends are planned.  Note these usually assume an underlying backend, not a direct implementation:
* SimpleBackend: Simply delegates tree data storage to a memory tree object, and supports applying transactions.  This implementation can stand alone, unlike most of the others.
* TransactionalBackend: Supportings building a transaction in memory, and then attempting to apply to the underlying backend.
* ThreadsafeBackend: Controls access to the underlying backend, so that the threadsafe backend can be used by multiple backends all used by different threads. It supports worker threads dealing with notifications to avoid blocking other notifications.
* NOT GOING TO IMPLEMENT: JournalingBackend: because as far as I know only the http3_client needs this, and only the http3_server_cpp supports this.  So rather than build journaling into the most general backend interface, the journaling concept will be implemented along with http3 and the usage of the transport layer.
* PostGRESBackend: Will also be a standalone implementation, and will present a PostGRES database as a Tree using the Backend interface.  But this will be developed in a separate project, not in WWATP project.
* CompositeBackend: Will allow mounting branches from other Backends as branches in a composed greater tree.
* HTTP3ClientBackend (Both cpp and javascript): Will keep a current cached tree, and track changes to it via journaling, and connect to a http3_server_cpp instance which can support querying the journal, and fast fowarding changes.

Currently, the following frontends are planned.  Most of these will not be done in the WWATP project, and these are indicated by (WWATP):
* DirectFrontend (WWATP): Is this actually even a thing, or is this just a backend?
* HTTP3ServerFrontend (WWATP): Will support querying the journal and fast forward in detail.  If the URL is not a tree query, will return a web page bootstrap which will run the DOMLinkFrontend.  The server frontend will provide CSS from the tree, supporting a LLMToolFrontend-> generated CSS.
* DOMLinkFrontend (WWATP): Connects to the javscript HTTP3ClientBackend, and maps tree nodes to the DOM with identifiers, callbacks, and classes from the CSS.  Probably will try to use a concept of DOM templating to expand a node.
* YAMLWatcherFrontend: Simultaneously watches a tree and a YAML file, and keeps them in sync. (no LLM needed)
* TopicWatcherFrontend: Simultaneously watches a tree and a text document, and keeps them in sync.  This is not always perfectly defined, and this is exactly the tool where an LLM would plug in nicely.
* LaTeXWatcherFrontend: Not sure.  Maybe this can work like an HTTP3ServerFrontend with respect to CSS, which LaTeX sort of has a parallel concept.
* WordDocumentFrontend: Also no sure.  Maybe the same concept.
* UnrealEngineFrontend: Assets, levels, scripts, all generated by LLMs and placed into the tree. This should be a very similiar concept to the DOMLinkFrontend.
* MaximaToolFrontend: Watches for math-like nodes that evoke evaluations, and posts evalutated results back to the tree.