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
- **Load Balancer Node**: From an IP standpoint, actually is the service at the IP address and port of the tree.  It's job is to redirect traffic flows, and so it possesses all the Authorization branches (and keeps nothing else) and knows which authorization instances are available to match and service them.
- **Client**: Conceptually, connects directly to a server (usually in fact a load balancer instance), and is linked into the client software as a library. Implementations must allow the server to provide proxy sources transparently, and must allow the linked software to control how much of the tree is kept and maintained in memory. 

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
  - Optional: TokenRing (A node containing the authors, permissions, rules, etc.)
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

## Underlying technologies

At present the Agents attaching to the tree will use QUIC and HTTP/3 communication.  This will permit, for example, different branches of the tree to be sent either whole or in part to multiple other agents simultaneously.

For now, the QUIC + HTTP/3 library will be nghttp3.