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

- [ ] Decide module format (CommonJS vs ESM). Prefer ESM for modern Node; interop noted.
- [ ] Add `package.json` with minimal scripts: lint, test, build (if using TS later), coverage.
- [ ] Add `.editorconfig`, `.gitignore` for JS artifacts.
- [ ] Choose and configure linter/formatter (ESLint + Prettier) – optional initially.
- [ ] Decide unit test framework (Jest or Vitest). Prefer Vitest for speed and ESM, or Jest for ubiquity.
- [ ] Node version target; ensure Buffer/TypedArray APIs used consistently.

## B. Core interfaces (mirroring C++)

1) Backend interface (`js_client_lib/interface/backend.js`)
- [ ] Define abstract class Backend with methods:
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
- [ ] Define Maybe<T> shape and helpers (Just/Nothing): simple tagged union.
- [ ] Define Notification, SequentialNotification shapes.
  - [ ] Ensure no Node-only dependencies in interface code (browser-safe).

2) TreeNode & related types (`js_client_lib/interface/tree_node.js`)
- [ ] Implement TreeNodeVersion with fields, comparisons, ++, defaults.
- [ ] Implement TreeNode with fields and methods similar to C++:
  - labelRule, description, propertyInfos [{type,name}], version, childNames, propertyData (byte buffer), queryHowTo?, qaSequence?
  - getters/setters; getNodeName/getNodePath
  - property helpers: getPropertyValue<T>, setPropertyValue<T>, insertProperty<T>, string/span variants.
  - prefixLabels/shortenLabels
  - YAML conversion helpers (optional if not immediately needed); at least stubs to maintain parity.
- [ ] Define Transaction-related typedefs:
  - NewNodeVersion = [Maybe<uint16>, [string, Maybe<TreeNode>]]
  - SubTransaction = [NewNodeVersion, NewNodeVersion[]]
  - Transaction = SubTransaction[]
  - Helpers to prefix/shorten transaction labels.
  - [ ] Confirm TreeNode propertyData is represented by Uint8Array for browser safety (avoid Node Buffer).

3) HTTP3TreeMessage & helpers
- Helpers (`js_client_lib/interface/http3_tree_message_helpers.js`)
  - [ ] Implement chunk model compatible with `shared_chunk.h`:
    - payload_chunk_header (signal_type=2, fields: signal, request_id, data_length)
    - signal_chunk_header (signal_type=1)
    - no_chunk_header (signal_type=0)
    - shared_span abstraction or a simplified equivalent that can:
      - carry header + payload
      - flatten with signal
      - provide size, get/set signal, request_id
    - chunks = shared_span[]
  - [ ] Implement encoders/decoders and can_decode_* for:
    - label (small string)
    - long_string (multi-chunk string)
    - Maybe<TreeNode>
    - SequentialNotification
    - Vector<SequentialNotification>
    - NewNodeVersion
    - SubTransaction
    - Transaction
    - Vector<TreeNode>
  - [ ] Ensure byte order and sizes match C++ (uint8, uint16, etc.).
  - [ ] Unit tests for each encoder/decoder.
  - [ ] Use Uint8Array/DataView, not Buffer, to support browsers. Add thin Buffer shim only in Node tests if needed.

- Message (`js_client_lib/interface/http3_tree_message.js`)
  - [ ] Implement class with state:
    - requestId, signal, isInitialized, isJournalRequest, requestComplete, responseComplete, processingFinished
    - requestChunks: chunk list, responseChunks: chunk list
  - [ ] Methods per C++ header:
    - encode/decode pairs for all backend methods; getJournal; static node request
    - pop/push Request/Response chunks
    - reset; setters; is* queries
  - [ ] Constructor, move-like resets, minimal validations.
  - [ ] Tests to validate round-trips.

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

- [ ] Use Uint8Array/DataView throughout; add small compatibility layer in tests when running under Node.
- [ ] Implement headers with exact field sizes (uint8, uint16 little-endian as per C++ layout); document endianness.
- [ ] Implement chunk size policy (1200 default); split long strings accordingly.
- [ ] Implement flatten_with_signal behavior for propertyData spans like C++ `flatten_with_signal`.
- [ ] Validate with golden vectors or cross-language fixture when available.

## G. Tests

- Unit tests for:
  - [ ] http3_tree_message_helpers encoders/decoders round trip
  - [ ] HTTP3TreeMessage request/response sequences (including journal)
  - [ ] TreeNode property operations and version comparisons
  - [ ] Backend interface conformance with a simple in-memory backend used as localBackend
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

- [ ] File: `js_client_lib/interface/simple_backend.js`
- [ ] Purpose: a basic in-memory tree backend implementing Backend for caching and offline ops; no transactions.
- [ ] API: implements all Backend methods; throws for openTransactionLayer/closeTransactionLayers if unsupported (like C++), applyTransaction modifies tree atomically in-memory.
- [ ] Data structures: Map<string, TreeNode> keyed by labelRule; child relationships maintained; query and page tree helpers.
- [ ] Listener registry: map of {listenerName+labelRule -> {childNotify, callback}} and notify semantics mirrored from C++.
- [ ] Serialization: not required (memory-only), but must handle TreeNode.propertyData Uint8Array consistently.
- [ ] Tests: cover CRUD, query, page tree, notifications, and transaction application behavior.

---

## K. Session summary (context)

Date: 2025-08-11

What we analyzed
- C++ headers shaping the JS port: backend.h, tree_node.h, http3_tree_message.h, http3_tree_message_helpers.h, http3_client_backend.h, frontend_base.h, transport/include/shared_chunk.h, transport/include/request.h, and memory/include/simple_backend.h.
- Existing JS files discovered: interface/*.js (empty stubs), transport/communication.js and quic_communication.js (Node-specific), and placeholders for http3_client.js and http3_client_updater.js.

Decisions
- Browser-first implementation: use Uint8Array/DataView, avoid Node-only APIs; design Communication adapters for WebTransport/WebSocket/fetch. Node QUIC stays optional.
- Keep current code as CommonJS initially; compatible with bundlers; revisit ESM if/when refactoring.
- Include a SimpleBackend JS port to support Http3ClientBackend caching and local operations in the browser.

Artifacts created/updated
- js_client_lib/TODO.md: exhaustive plan with sections A–J, browser constraints integrated, SimpleBackend section added.
- js_client_lib/package.json: minimal scaffold with vitest and basic scripts.

Open items (next steps candidates)
- Add Vitest config (optional) and first unit test for encoding helpers using Uint8Array/DataView.
- Implement Maybe helper, Request type, and StreamIdentifier in JS.
- Start http3_tree_message_helpers with headers layout and label encoder/decoder, then build up.
- Sketch Communication interfaces for WebTransport and WebSocket adapters.
