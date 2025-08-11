# HTTP3ClientBackend JS Port – Project TODO

This document tracks the end-to-end tasks for porting the C++ HTTP3 client backend to JavaScript, aligned to the C++ header contracts under `common/backend_cpp/interface` and `common/backend_cpp/http3_client` and the transport layer in `common/transport/include`.

## Scope and parity goals

- Feature parity with C++ types and APIs exposed in:
  - interface/backend.h (Backend interface)
  - interface/tree_node.h (TreeNode, TreeNodeVersion, Transaction types)
  - interface/http3_tree_message.h + http3_tree_message_helpers.h (wire encoding)
  - http3_client/http3_client_backend.h (client backend orchestration + updater)
  - transport/include/shared_chunk.h (chunk headers and framing) – JS equivalent API & behaviors
  - transport/include/request.h (Request)
  - frontend/frontend_base.h (Frontend base shape – used by Updater)
- Support both blocking and non-blocking modes for applyTransaction and request/response wait semantics.
- Implement journaling: client-initiated polling (“solicit journal request”) and response processing to keep local backend in sync.
- Implement listener registration and notifications.
- Provide a working transport abstraction; QUIC implementation optional behind an interface.
- Provide tests mirroring C++ behaviors where practical.

Browser runtime constraints
- Target: must run in web browsers (the test framework may be Node-only). Make all design choices browser-first.
- Transport in browsers:
  - Prefer WebTransport (HTTP/3 over QUIC) when available; provide an adapter behind `Communication`.
  - Fallback to fetch/XHR or WebSocket for dev mode; still use chunk/message framing to preserve protocol semantics when possible.
  - If QUIC specifics are required, consider WebAssembly bridge for framing only; avoid Node-only modules.
- Avoid Node-specific APIs in runtime code (Buffer, net, fs). Use Uint8Array/DataView and Web APIs.
- Keep JS modules ESM-friendly for browser bundlers (Vite/Rollup). Provide minimal shims for Node tests.

---

## A. Project structure and scaffolding

- [x] Decide module format (CommonJS vs ESM). Prefer ESM for modern Node; interop noted.
- [x] Add `package.json` with minimal scripts: lint, test, build (if using TS later), coverage.
- [x] Add `.editorconfig`, `.gitignore` for JS artifacts.
- [x] Choose and configure linter/formatter (ESLint + Prettier) – optional initially.
- [x] Decide unit test framework (Jest or Vitest). Prefer Vitest for speed and ESM, or Jest for ubiquity.
- [x] Node version target; ensure Buffer/TypedArray APIs used consistently.

## B. Core interfaces and SimpleBackend (mirroring C++)

1) Backend interface (`js_client_lib/interface/backend.js`)
 - [x] Define abstract class Backend with methods:
  - getNode(labelRule): Promise<Maybe<TreeNode>> or sync for local wrappers
  - upsertNode(nodes: TreeNode[]): Promise<boolean>
  - deleteNode(labelRule: string): Promise<boolean>
  - getPageTree(pageNodeLabelRule: string): Promise<TreeNode[]>
  - relativeGetPageTree(node: TreeNode, pageNodeLabelRule: string): Promise<TreeNode[]>
  - queryNodes(labelRule: string): Promise<TreeNode[]>
  - relativeQueryNodes(node: TreeNode, labelRule: string): Promise<TreeNode[]>
  - openTransactionLayer(node: TreeNode): Promise<boolean>
  - closeTransactionLayers(): Promise<boolean>
  - applyTransaction(tx: Transaction): Promise<boolean>
  - getFullTree(): Promise<TreeNode[]>
  - registerNodeListener(listenerName: string, labelRule: string, childNotify: boolean, cb: (backend, labelRule, maybeNode) => void): void
  - deregisterNodeListener(listenerName: string, labelRule: string): void
  - notifyListeners(labelRule: string, maybeNode: Maybe<TreeNode>): void
  - processNotifications(): void
 - [x] Define Maybe<T> shape and helpers (Just/Nothing): simple tagged union.
 - [x] Define Notification, SequentialNotification shapes.
  - [x] Ensure no Node-only dependencies in interface code (browser-safe).

2) TreeNode & related types (`js_client_lib/interface/tree_node.js`)

 - [x] File scaffold and exports:
   - [x] Create `interface/tree_node.js` exporting: TreeNodeVersion, TreeNode, fixedSizeTypes, fromYAMLNode (stub), toYAML (stub), and Transaction helpers.
   - [x] Import Maybe helpers from `interface/maybe.js` and re-export types used by other modules when helpful.

 - [x] TreeNodeVersion (C++ parity):
   - [x] Fields: versionNumber:uint16 (default 0), maxVersionSequence:uint16 (default 256), policy:string (default "default"), authorialProof:Maybe<string>, authors:Maybe<string>, readers:Maybe<string>, collisionDepth:Maybe<number>.
   - [x] Methods:
     - [x] increment() to wrap with modulo maxVersionSequence.
     - [x] isDefault() check as per C++ defaults.
     - [x] Comparison operators with wrap-around semantics: lt/le/gt/ge/eq using the half-range rule from C++.
   - [x] Serialization helpers (optional): toJSON()/fromJSON() and YAML stubs; validate policy non-empty on encode.
   - [x] Tests: eq/lt/gt around wrap boundaries; isDefault; increment wrap.

 - [x] TreeNode API and data model:
   - [x] Fields:
     - [x] labelRule:string; description:string; propertyInfos:Array<{type:string,name:string}>.
     - [x] version:TreeNodeVersion; childNames:string[].
     - [x] propertyData:Uint8Array (browser-safe, no Buffer).
     - [x] queryHowTo:Maybe<string>; qaSequence:Maybe<string>.
   - [x] Getters/setters for all fields.
   - [x] Path helpers: getNodeName() (last segment after '/'), getNodePath() (prefix path before last '/'), getAbsoluteChildNames() (resolve children to absolute paths relative to node path).
   - [x] Operator++ equivalent: bump version via version.increment().
   - [x] Equality: deep comparison of all fields including byte-wise equality of propertyData.
   - [x] Label prefix helpers: prefixLabels(prefix), shortenLabels(prefix) applied to labelRule and childNames.

 - [x] Property data layout and helpers (binary correctness):
   - [x] fixedSizeTypes = {"int64","uint64","double","float","bool"} to mirror C++ set.
   - [x] Encoding policy in propertyData:
     - [x] For fixed-size types, use little-endian DataView encodings: int64/uint64 via get/setBig(Int)64, double via get/setFloat64, float via get/setFloat32, bool as 1-byte 0/1.
     - [x] For variable-size types (e.g., "string","text","yaml","png"), store 32-bit little-endian length prefix followed by raw bytes.
     - [x] Compute offsets by scanning propertyInfos in order, summing fixed sizes or reading length prefixes for variable sizes.
   - [x] Implement helpers:
     - [x] getPropertyValue<T>(name): returns [size:number, value:T, raw:Uint8Array].
     - [x] getPropertyString(name): [size:number, value:string] using UTF-8.
     - [x] getPropertyValueSpan(name): [size:number, header:Uint8Array, data:Uint8Array] for variable-size blobs.
     - [x] setPropertyValue<T>(name, value): in-place rebuild of propertyData maintaining property order; throw if name not found.
     - [x] setPropertyString(name, value) and setPropertyValueSpan(name, data:Uint8Array).
     - [x] insertProperty(index, name, value<T>), insertPropertyString(index, name, type, value), insertPropertySpan(index, name, type, data:Uint8Array) with append-on-out-of-range behavior; throw if name already exists.
     - [x] deleteProperty(name) updates propertyInfos and compacts propertyData.
   - [x] UTF-8 helpers for string<->Uint8Array round trips; do not use Node Buffer.
   - [x] Tests: construct several property layouts, round-trip reads, updates, inserts at head/middle/tail, deletes; verify sizes and byte layout.

 - [x] YAML parity (optional now):
   - [x] toYAML/fromYAML stubs mirroring signatures; no external YAML dependency required initially.
   - [x] Document that binary property layout is maintained; YAML conversion for properties may be deferred.

 - [x] Transaction types and helpers (tuple parity with C++):
   - [x] Type aliases (documented shapes):
     - [x] NewNodeVersion = [Maybe<number /*uint16*/>, [string /*labelRule*/, Maybe<TreeNode>]]
     - [x] SubTransaction = [NewNodeVersion, NewNodeVersion[]]
     - [x] Transaction = SubTransaction[]
   - [x] Helper functions:
     - [x] prefixNewNodeVersionLabels(prefix, nnv); shortenNewNodeVersionLabels(prefix, nnv)
     - [x] prefixSubTransactionLabels(prefix, st); shortenSubTransactionLabels(prefix, st)
     - [x] prefixTransactionLabels(prefix, tx); shortenTransactionLabels(prefix, tx)
   - [x] Tests: create sample transactions, run prefix/shorten helpers, validate label changes on parents and descendants; ensure immutability or clearly document mutation.

 - [x] Browser-safety and interop:
   - [x] Confirm all APIs are ESM and browser friendly; no Node Buffer/fs/net.
   - [x] Use Uint8Array/DataView exclusively; gate BigInt64/BigUint64 usage with feature checks and fallback to polyfills in tests if needed.

3) SimpleBackend (`js_client_lib/interface/simple_backend.js`)

 - [x] Implement class SimpleBackend that conforms to Backend (sync methods are acceptable; callers may `await` sync returns).
 - [x] Data model: Map<string, TreeNode> keyed by labelRule; maintain child relationships for queries and notifications.
 - [x] Query semantics:
   - [x] Implement partial/overlap label-rule matching helpers analogous to C++ `partialLabelRuleMatch` and `checkLabelRuleOverlap`.
   - [x] Implement `queryNodes(labelRule)` using these helpers; include relativeQueryNodes.
 - [x] Page tree semantics:
   - [x] Implement `getPageTree(pageNodeLabelRule)` and `relativeGetPageTree(node, pageNodeLabelRule)`.
   - [x] Decide minimal interoperable representation for page nodes’ list of label rules in `propertyData` (UTF-8 JSON array for tests is acceptable); document it in code comments.
 - [x] Transactions:
   - [x] `openTransactionLayer` and `closeTransactionLayers` throw UnsupportedOperation (mirroring C++ SimpleBackend behavior).
   - [x] `applyTransaction(tx)` applies subtransactions atomically in-memory; validate before mutate, then commit.
 - [x] Listeners:
   - [x] Implement `registerNodeListener(listenerName, labelRule, childNotify, cb)` and `deregisterNodeListener`.
   - [x] Implement `notifyListeners(labelRule, maybeNode)`; when `childNotify` is true, notify on children that partially match listener label-rule (use the helpers above).
   - [x] `processNotifications()` is a noop for this backend.
 - [x] Serialization: ensure `TreeNode.propertyData` is Uint8Array; provide UTF-8 encode/decode helpers for tests.
 - [x] Acceptance tests (see B.4): CRUD, queries, page tree, transactions, and listener notifications must match C++ SimpleBackend intent.

4) Backend Testbed (JS parity suite)

 - [x] Location: place JS testbed under `js_client_lib/test/backend_testbed/`:
   - [x] `backend_testbed.js`: reusable helpers mirroring C++ testbed utilities.
   - [x] `backend_testbed.test.js`: Vitest suite that runs the helpers against a Backend implementation.
 - [x] Port testbed helpers inspired by C++ `test_instances/catch2_unit_tests/backend_testbed.h`:
   - [x] `createNoContentTreeNode(labelRule, description, propertyInfos, version, childNames, queryHowTo?, qaSequence?)`
   - [x] `createAnimalNode(animal, description, propertyInfos, version, childNames, propertyDataStrings[], queryHowTo, qaSequence)`
   - [x] `createAnimalDossiers(animalNode)`; `createLionNodes()`; `createElephantNodes()`; `createParrotNodes()`
   - [x] `collectAllNotes()`; `createNotesPageTree()`; `prefixNodeLabels(prefix, nodes)`
   - [x] `checkGetNode(backend, labelRule, expectedNode)`; `checkMultipleGetNode(backend, expectedNodes)`
   - [x] `checkDeletedNode(backend, labelRule)`; `checkMultipleDeletedNode(backend, expectedNodes)`
 - [ ] Implement `BackendTestbed` class for JS with methods analogous to C++:
   - [x] constructor(backend, { shouldTestNotifications = true, shouldTestChanges = true })
   - [x] `addAnimalsToBackend()`; [x] `addNotesPageTree()`; [ ] `stressTestConstructions(count)`
   - [ ] `testAnimalNodesNoElephant(labelPrefix = "")`;
   - [x] `testBackendLogically(labelPrefix = "")`;
   - [ ] `testPeerNotification(toBeModified, notificationDelayMs, labelPrefix = "")`
   - Note: `stressTestConstructions` and `testPeerNotification` are deferred for now due to low utility in typical web-browser environments (no real multithreading, timing semantics differ). Can be added later if needed.
 - [ ] Write a Vitest suite that runs the testbed against:
   - [x] `SimpleBackend` (B.3) to validate local behavior.
   - [ ] Later: `Http3ClientBackend` once transport/messages are available (same suite should run unchanged for parity).
 - [ ] Align expected behaviors to C++ semantics:
   - [ ] Listener notifications fire after version changes or deletions; for `childNotify`, use partial label matches.
   - [ ] Transactions apply atomically; invalid subtransactions must not partially mutate state.
   - [ ] Page-tree expansion must resolve all listed label rules and aggregate results deterministically for assertions.

5) HTTP3TreeMessage helpers
- Helpers (`js_client_lib/interface/http3_tree_message_helpers.js`)
  - [x] Implement chunk model compatible with `shared_chunk.h`:
    - payload_chunk_header (signal_type=2, fields: signal, request_id, data_length)
    - signal_chunk_header (signal_type=1)
    - no_chunk_header (signal_type=0)
    - shared_span abstraction or a simplified equivalent that can:
      - carry header + payload
      - flatten with signal
      - provide size, get/set signal, request_id
    - chunks = shared_span[]
  - [x] Implement encoders/decoders and can_decode_* for:
    - label (small string)
    - long_string (multi-chunk string)
    - Maybe<TreeNode>
    - SequentialNotification
    - Vector<SequentialNotification>
    - NewNodeVersion
    - SubTransaction
    - Transaction
    - Vector<TreeNode>
  - [x] Ensure byte order and sizes match C++ (uint8, uint16, etc.).
  - [x] Unit tests for each encoder/decoder.
  - [x] Use Uint8Array/DataView, not Buffer, to support browsers. Add thin Buffer shim only in Node tests if needed.

- HTTP3TreeMessage (`js_client_lib/interface/http3_tree_message.js`)
  - [x] Implement class with state:
    - requestId, signal, isInitialized, isJournalRequest, requestComplete, responseComplete, processingFinished
    - requestChunks: chunk list, responseChunks: chunk list
  - [x] Methods per C++ header (updated):
    - encode/decode pairs for all backend methods; getJournal; static node request
    - pop/push Request/Response chunks
    - reset; setters; is* queries
  - [x] Operation encode/decode coverage (aligned with tests):
    - [x] encode/decode getNodeRequest
    - [x] encode/decode upsertNodeRequest
  - [x] Operation encode/decode remaining (copied from test TODOs):
    - [x] encode and decode deleteNodeRequest
    - [x] encode and decode getPageTreeRequest
    - [x] encode and decode getQueryNodesRequest
    - [x] encode and decode openTransactionLayerRequest
    - [x] encode and decode closeTransactionLayersRequest
    - [x] encode and decode applyTransactionRequest
    - [x] encode and decode getFullTreeRequest
    - [x] encode and decode registerNodeListenerRequest
    - [x] encode and decode deregisterNodeListenerRequest
    - [x] encode and decode notifyListenersRequest
    - [x] encode and decode processNotificationRequest
    - [x] encode and decode getJournalRequest
  - [x] Constructor, move-like resets, minimal validations.
  - [x] Tests to validate round-trips.

## C. Transport abstraction (`js_client_lib/transport`)

- [ ] Define Communication base (`communication.js`) mirroring C++ responsibilities:
  - getNewRequestStreamIdentifier(req): StreamIdentifier
  - registerResponseHandler(sid, cb)
  - hasResponseHandler(sid)
  - deregisterResponseHandler(sid)
  - processRequestStream(): boolean
  - registerRequestHandler(named_prepare_fn)
  - deregisterRequestHandler(name)
  - processResponseStream(): boolean
  - listen(), close(), connect()
- [ ] Define StreamIdentifier shape: { cid, logicalId } with equality and ordering helpers if needed.
- [ ] Request shape mirroring `request.h` with isWWATP().
- [ ] Browser-first transports:
  - WebTransport adapter (preferred when available in the environment)
  - WebSocket adapter as fallback (frame WWATP chunks into binary messages)
  - Fetch/XHR fallback for request/response patterns that can be emulated (non-streaming)
- [ ] Optional Node-only adapters kept out of browser bundle (e.g., `quic_communication.js`) guarded by environment.
- [ ] Provide a mock/in-memory transport for unit tests that exercises chunk framing.

## D. HTTP3 client backend in JS

1) Http3ClientBackend (`js_client_lib/http3_client.js`)
- [ ] Class constructor signature mirrors C++:
  - constructor(localBackend, blockingMode, request, journalRequestsPerMinute = 0, staticNode = Nothing)
  - validations per C++ (staticNode with WWATP check, journaling with static node check)
  - if staticNode present and !blockingMode -> requestStaticNodeData()
- [ ] Implement methods:
  - getStaticNode(): Maybe<TreeNode>
  - getNode(labelRule)
  - upsertNode(nodes)
  - deleteNode(labelRule)
  - getPageTree(pageNodeLabelRule)
  - relativeGetPageTree(node, pageNodeLabelRule)
  - queryNodes(labelRule)
  - relativeQueryNodes(node, labelRule)
  - openTransactionLayer(node)
  - closeTransactionLayers()
  - applyTransaction(transaction) – block in blockingMode until server response
  - getFullTree()
  - registerNodeListener(listenerName, labelRule, childNotify, cb)
  - deregisterNodeListener(listenerName, labelRule)
  - notifyListeners(labelRule, maybeNode)
  - processNotifications()
  - processHTTP3Response(http3TreeMessage)
  - hasNextRequest(), popNextRequest()
  - solicitJournalRequest()
  - getRequestUrlInfo()
  - requestFullTreeSync(), setMutablePageTreeLabelRule(label="MutableNodes"), getMutablePageTree()
  - needToSendJournalRequest(time), setJournalRequestComplete()
- [ ] Internal helpers:
  - requestStaticNodeData(), setNodeChunks(chunks)
  - getMutablePageTreeInMode(blockingMode, isJournalRequest=false)
  - awaitResponse()/awaitBoolResponse()/awaitTreeResponse()/awaitVectorTreeResponse()/awaitChunksResponse()
  - responseSignal()
  - processOneNotification(notification), updateLastNotification(notification)
  - enable/disable server sync flags, notification blocking flag
  - queues: pendingRequests_ (list)
  - state: needMutablePageTreeSync_, lastNotification_, mutablePageTreeLabelRule_, serverSyncOn_, blockingMutex/condition -> JS equivalents (Promises + deferreds)
  - journaling: lastJournalRequestTime, journalRequestWaiting, rate-limiting per minute
  - [ ] Ensure all code paths are browser-safe (no Node-specific APIs); timers and queues use Web APIs.

2) Http3ClientBackendUpdater (`js_client_lib/http3_client_updater.js`)
- [ ] Constructor(name, ipAddr, port); name/type getters
- [ ] addBackend(localBackend, blockingMode, request, journalRequestsPerMinute=0, staticNode)
- [ ] getBackend(url) and getBackends()
- [ ] maintainRequestHandlers(connector, time):
  - while backends have pending requests, request new stream identifiers
  - create HTTP3TreeMessage handlers for each request; map StreamIdentifier -> message
  - periodically solicit journal requests from each backend; map journaling streams separately
- [ ] start(connector, time, sleepMilli=100) -> spawn loop equivalent (setInterval or worker thread)
- [ ] stop(), isRunning(), size()
- [ ] Track: ongoingRequests (Map), journalingRequests (Map), lastTime, completeRequests queue
- [ ] Integration with Communication.processRequestStream()
 - [ ] For browser, use setInterval/requestAnimationFrame loops; ensure stop() clears timers.

## E. Concurrency and blocking semantics

- [ ] Replace C++ mutex/condition_variable with JS constructs:
  - For blockingMode waits, use Promises and resolvers per requestId; a wait map keyed by requestId.
  - Ensure processHTTP3Response resolves the proper pending promise.
  - Avoid deadlocks; timeouts/logging for diagnostics.
- [ ] Ensure thread safety assumptions: Node event loop single-threaded; if workers used, message passing only.
 - [ ] Browser: avoid SharedArrayBuffer/Atomics unless absolutely necessary and cross-origin isolation is configured. Prefer event-driven waits.

## F. Serialization details and binary safety

- [x] Use Uint8Array/DataView throughout; add small compatibility layer in tests when running under Node.
- [x] Implement headers with exact field sizes (uint8, uint16 little-endian as per C++ layout); document endianness.
- [x] Implement chunk size policy (1200 default); split long strings accordingly.
- [x] Implement flatten_with_signal behavior for propertyData spans like C++ `flatten_with_signal`.
- [ ] Validate with golden vectors or cross-language fixture when available.

## G. Tests

- Unit tests for:
  - [x] http3_tree_message_helpers encoders/decoders round trip
  - [x] HTTP3TreeMessage request/response sequences (including journal)
Decisions (updated)
  - [x] Backend interface conformance with a simple in-memory backend used as localBackend
  - [ ] Http3ClientBackend behavior: pending queue, blocking waits, journal rate-limiting, static node fetching, listener notifications
  - [ ] Updater maintainRequestHandlers flow with a mock Communication
  - [ ] Transport mock: stream identifier management, response handler routing
- [ ] Minimal integration test: issue getFullTree or queryNodes over mock transport and observe localBackend updates.
- [ ] Browser-run tests (e.g., via Vitest + jsdom or Karma) for adapters; Node-only tests acceptable for heavier unit coverage.

## H. Docs and examples

- [ ] README in `js_client_lib` with quick-start, architecture diagram, notes on parity vs C++.
- [ ] Usage example: constructing backend, updater, and making a call.
- [ ] Notes on QUIC availability and using mock transport.
- [ ] Browser usage: bundler example (Vite), WebTransport capability detection, fallbacks.

## I. Stretch and future work

- [ ] TypeScript typings or full migration to TS.
- [ ] Browser build (WebTransport) with appropriate transport adapter.
- [ ] Backpressure and flow control tuning for large trees.
- [ ] Performance benchmarks vs C++ framing.

---

## Cross-reference to C++ headers

- backend.h -> interface/backend.js
- tree_node.h -> interface/tree_node.js (+ transactions helpers)
- http3_tree_message.h -> interface/http3_tree_message.js
- http3_tree_message_helpers.h -> interface/http3_tree_message_helpers.js
- shared_chunk.h -> helpers chunk model (headers + span abstraction)
- request.h -> simple JS Request type used by updater and transport
- http3_client_backend.h -> http3_client.js + http3_client_updater.js
- frontend_base.h -> updater base-like behaviors
 - memory/include/simple_backend.h -> SimpleBackend JS port (see below)

## Acceptance criteria

- All methods in Backend interface implemented in Http3ClientBackend with correct request/response wiring.
- Journaling works with rate-limiting and updates a local in-memory backend in tests.
- Encoder/decoder round-trip tests pass for all supported types.
- Updater can maintain request handlers and process streams over a mock transport.
- Code passes lint/tests and includes minimal documentation.

---

## J. SimpleBackend JS port (in-memory backend)

Port the C++ SimpleBackend to support the Http3ClientBackend locally in the browser.

 - [x] File: `js_client_lib/interface/simple_backend.js`
 - [x] Purpose: a basic in-memory tree backend implementing Backend for caching and offline ops; no transaction layers; applyTransaction supported.
 - [x] API: implements all Backend methods; throws for openTransactionLayer/closeTransactionLayers if unsupported (like C++), applyTransaction modifies tree atomically in-memory.
 - [x] Data structures: Map<string, TreeNode> keyed by labelRule; child relationships maintained; query and page tree helpers.
 - [x] Listener registry: map of {listenerName+labelRule -> {childNotify, callback}} and notify semantics mirrored from C++.
 - [x] Serialization: not required (memory-only), but must handle TreeNode.propertyData Uint8Array consistently.
 - [x] Tests: cover CRUD, query, page tree, notifications, and transaction application behavior.

---

## K. Session summary (context)

Date: 2025-08-11

What we analyzed
- C++ headers shaping the JS port: backend.h, tree_node.h, http3_tree_message.h, http3_tree_message_helpers.h, http3_client_backend.h, frontend_base.h, transport/include/shared_chunk.h, transport/include/request.h, and memory/include/simple_backend.h.
- Existing JS files discovered: interface/*.js (maybe.js, backend.js), transport scaffolding; tree_node.js implementation and tests in place.

Decisions
- Browser-first implementation: use Uint8Array/DataView, avoid Node-only APIs; design Communication adapters for WebTransport/WebSocket/fetch. Node QUIC stays optional and is stubbed as Node-only.
- ESM for js_client_lib (type: module) to align with browser bundlers and modern Node; avoid CommonJS for runtime code.
- Include a SimpleBackend JS port to support Http3ClientBackend caching and local operations in the browser.

Artifacts created/updated (this iteration)
- js_client_lib/interface/http3_tree_message_helpers.js: Implemented chunk model (headers + SpanChunk) and encoders/decoders for label, long_string, Maybe<TreeNode>, SequentialNotification, Vector<SequentialNotification>, NewNodeVersion, SubTransaction, Transaction, and Vector<TreeNode>; added flattenWithSignal utilities.
- js_client_lib/interface/http3_tree_message.js: Implemented full HTTP3TreeMessage encoders/decoders for all operations (getNode, upsertNode, deleteNode, getPageTree, queryNodes, open/close transaction layers, applyTransaction, getFullTree, register/deregister/notify listeners, processNotification, getJournal), with state and chunk helpers.
- js_client_lib/test/http3_tree_message_helpers.test.js: Parity tests for helpers (labels, long strings, Maybe<TreeNode>, transactions, vector<TreeNode>, sequential notifications, and chunk split/collect).
- js_client_lib/test/http3_tree_message.test.js: Added full round-trip parity tests for all HTTP3TreeMessage operations, including journal flows.
- js_client_lib/test/backend_testbed/backend_testbed.js and *.test.js: Core helpers and logical test flow against SimpleBackend.
- Prior artifacts retained: tree_node.js + tests; ESLint config; package.json scripts; index.js exports updated.

Status updates
- Section A: complete.
- Section B.1 (Backend interface) and Maybe<T>: complete.
- Section B.2 (TreeNode & related types): complete with tests.
- Section B.3 (SimpleBackend): complete with tests; cascade-delete added.
- Section B.4 (Backend Testbed): core helpers and logical test flow implemented; suite runs against SimpleBackend and passes.
- Section B.5 (HTTP3TreeMessage & helpers): helpers complete with tests; HTTP3TreeMessage now has full encode/decode coverage with passing round-trip tests across all operations.

Notes / deferrals
- `stressTestConstructions` and `testPeerNotification` are intentionally deferred for now due to low utility in browser-first context; can be implemented later if needed.
- HTTP3TreeMessage full method coverage and round-trip tests remain.

Open items (next steps candidates)
- Define Request/StreamIdentifier shapes for transport abstraction.
- Add optional notification-focused tests in the testbed when transport/journaling paths are available.
- Broaden unit tests (message round-trips, transport mock, integration).
