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
 - [x] Implement `BackendTestbed` class for JS with methods analogous to C++:
   - [x] constructor(backend, { shouldTestNotifications = true, shouldTestChanges = true })
   - [x] `addAnimalsToBackend()`; [x] `addNotesPageTree()`; [ ] `stressTestConstructions(count)`
   - [ ] `testAnimalNodesNoElephant(labelPrefix = "")`;
   - [x] `testBackendLogically(labelPrefix = "")`;
  - [ ] `testPeerNotification(toBeModified, notificationDelayMs, labelPrefix = "")`
  - Note: `stressTestConstructions` remains deferred for browser-first contexts. For peer notifications, we will prioritize a system-level test using two Http3ClientBackend instances (see section G) even if the unit-level testbed method stays deferred.
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

- [x] Define Communication base (`communication.js`) mirroring C++ responsibilities:
  - getNewRequestStreamIdentifier(req): StreamIdentifier
  - registerResponseHandler(sid, cb)
  - hasResponseHandler(sid)
  - deregisterResponseHandler(sid)
  - processRequestStream(): boolean
  - registerRequestHandler(named_prepare_fn)
  - deregisterRequestHandler(name)
  - processResponseStream(): boolean
  - listen(), close(), connect()
- [x] Define StreamIdentifier shape: { cid, logicalId } with equality and ordering helpers if needed.
- [x] Request shape mirroring `request.h` with isWWATP().
- [x] Browser-first transports:
  - [x] WebTransport adapter (preferred when available in the environment)
  - [ ] WebSocket adapter as fallback (frame WWATP chunks into binary messages)
  - [x] Fetch/XHR fallback for request/response patterns that can be emulated (non-streaming)
- [x] Optional Node-only adapters kept out of browser bundle (e.g., `quic_communication.js`) guarded by environment.
- [x] Provide a mock/in-memory transport for unit tests that exercises chunk framing.

- [x] Node-only curl bridge (short-term real-server path)
  - [x] File: `js_client_lib/transport/curl_communication.js` implementing the Communication interface.
  - [x] Execute `curl --http3` per request (child_process) to POST WWATP chunk-framed bytes to `/init/wwatp/`; read binary response from stdout.
  - [x] Map TLS options from env/per-request: `WWATP_CERT`, `WWATP_KEY`, `WWATP_CA`, `WWATP_INSECURE=1`.
  - [x] Non-streaming only: treat responses as complete bodies (RESPONSE_FINAL); document limitation (no server push/streaming).
  - [x] Select via env `WWATP_TRANSPORT=curl`; documented to avoid bundling into browsers.

- [x] Node-only libcurl HTTP/3 transport (single-connection reuse)
  - [x] File: `js_client_lib/transport/libcurl_transport.js` implementing the Communication interface.
  - [x] Force HTTP/3 and reuse a single QUIC connection via node-libcurl; open new streams for follow-ups (e.g., heartbeats).
  - [x] Map TLS options from env: `WWATP_CERT`, `WWATP_KEY`, `WWATP_CA`, `WWATP_INSECURE=1`.
  - [x] Prefer in real-server tests; skip gracefully if `node-libcurl` is not installed.

## D. HTTP3 client backend in JS

1) Http3ClientBackend (`js_client_lib/http3_client.js`)
- [x] Class constructor signature mirrors C++:
  - constructor(localBackend, blockingMode, request, journalRequestsPerMinute = 0, staticNode = Nothing)
  - validations per C++ (staticNode with WWATP check, journaling with static node check)
  - if staticNode present and !blockingMode -> requestStaticNodeData() [present; see fix task below]
- [x] Implement methods:
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
  - [x] requestStaticNodeData(): create request for static URL and send it; when response arrives, load returned asset into staticNode_ and update local cache.
  - [ ] setNodeChunks(chunks) (if needed for chunk-level flows)
  - [ ] getMutablePageTreeInMode(blockingMode, isJournalRequest=false)
  - [x] awaitBoolResponse()/awaitTreeResponse()/awaitVectorTreeResponse() (minimal promise-based)
  - [ ] awaitResponse()/awaitChunksResponse() and responseSignal() handling
  - [x] processOneNotification(notification), updateLastNotification(notification)
  - [ ] enable/disable server sync flags, notification blocking flag helpers
  - [x] queues: pendingRequests_ (list)
  - [x] state: needMutablePageTreeSync_, lastNotification_, mutablePageTreeLabelRule_, serverSyncOn_ (flag), promise-based waits
  - [x] journaling: lastJournalRequestTime, journalRequestWaiting, rate-limiting per minute
  - [x] Ensure all code paths are browser-safe (no Node-specific APIs); timers and queues use Web APIs.

- [ ] processHTTP3Response coverage tasks (remaining cases to implement/verify):
  - [x] Handle RESPONSE_CONTINUE/RESPONSE_FINAL chunk sequencing and partial payload assembly where applicable (multi-chunk responses).
  - [ ] Handle empty/signal-only responses uniformly (e.g., processNotification returning no payload) and resolve waits accordingly.
  - [ ] Handle unexpected/unknown signals with a clear error/log path without breaking the wait map.
  - [ ] Journal edge case: when server returns the mutable page tree as the response to a journal request due to client being out of sync; wire this into local cache and lastNotification_ correctly.
  - [x] Static asset response path: route static URL responses to update staticNode_ and optionally notify local listeners.
  - [x] Compatibility with curl bridge: resolve waits on final body parse and avoid assumptions about streaming.

2) Http3ClientBackendUpdater (`js_client_lib/http3_client_updater.js`)
- [x] Constructor(name, ipAddr, port); name/type getters
- [x] addBackend(localBackend, blockingMode, request, journalRequestsPerMinute=0, staticNode)
- [x] getBackend(url) and getBackends()
- [x] maintainRequestHandlers(connector, time):
  - [x] while backends have pending requests, request new stream identifiers
  - [x] create HTTP3TreeMessage handlers for each request; map StreamIdentifier -> message
  - [x] periodically solicit journal requests from each backend; map journaling streams separately
- [x] start(connector, time, sleepMilli=100) -> spawn loop equivalent (setInterval or worker thread)
- [x] stop(), isRunning(), size()
- [x] Track: ongoingRequests (Map), lastTime (journalingRequests/completeRequests optional; tracked implicitly)
- [x] Integration with Communication.processRequestStream()
 - [x] For browser, use setInterval loops; ensure stop() clears timers.

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
  - [x] Updater maintainRequestHandlers flow with a mock Communication
    - [x] Transport mock: stream identifier management, response handler routing
 - [x] Minimal integration test: issue getFullTree or queryNodes over mock transport and observe localBackend updates.
 - [x] Real-server smoke using curl bridge:
   - [x] When `WWATP_TRANSPORT=curl` and `WWATP_E2E=1`, instantiate Updater with curl transport and perform a minimal request (e.g., getFullTree or getNode) against `/init/wwatp/`; assert decode and local cache update.
   - [x] Skip with message if `curl --http3` is unavailable; provide hint to install a curl with HTTP/3.
 
### System-level tests

Group A – Mock transport (implemented)

- [x] End-to-end with mock transport and in-memory server:
  - Files: `test/system/server_mock.js`, `test/system/system_mock_transport.test.js`
  - Validates the idiom: add animals + add notes + test backend logically using a shared SimpleBackend server and two JS clients.
  - Covers two-client peer notification via periodic journal polling. A registers a listener on 'lion', B modifies server; A observes create and delete notifications in order; after deregistration A sees no more notifications.

Group B – Real WWATPService server (scaffolded)

- [x] Spawn real server binary and run tests against it:
  - Files: `test/system/system_real_server.test.js` (skipped by default), `test/resources/wwatp_server_config.yaml`.
  - How it will work:
    1) Build the C++ server (`build/wwatp_server`).
    2) Launch it with the provided YAML.
    3) Connect JS Http3ClientBackend(s) to the HTTP/3 endpoint and run: add animals + add notes + test backend logically, and peer notification with two clients.
  - Current status: When `WWATP_E2E=1` and `node-libcurl` is available, the suite runs WWATP flows using the single-connection LibcurlTransport and passes. The curl CLI probe remains for static assets; tests skip gracefully if prerequisites are missing.
  - Documentation requirement: For every investigation and outcome in Group B below, record the answers and procedures in `js_client_lib/README.md` under a "Real server e2e" section so the setup is reproducible.

Integration readiness and investigation tasks

- Server binary + config smoke checks
  - [x] Validate config only: run `build/wwatp_server --check-only js_client_lib/test/resources/wwatp_server_config.yaml` and expect exit code 0 and output containing "Configuration validation completed successfully".
  - [x] Runtime smoke: start `build/wwatp_server js_client_lib/test/resources/wwatp_server_config.yaml` with cwd at repo root; wait for UDP bind; then stop; clean shutdown and non-error stderr.
  - [x] Verify listener: confirm UDP QUIC port from YAML (default 12345) is bound. If occupied, change YAML (env override deferred).

- Config YAML and required files correctness
  - [x] Confirm relative paths in `wwatp_server_config.yaml` resolve from repo root (test uses cwd=ROOT):
        - `private_key_file: ../../test_instances/data/private_key.pem`
        - `cert_file: ../../test_instances/data/cert.pem`
        - `log_path: ../../test_instances/sandbox/`
        - static assets: `index.html`, `randombytes.bin`
  - [x] Ensure `test_instances/sandbox/` exists prior to startup; create if missing.
  - [x] Ensure `test_instances/data/cert.pem` and `test_instances/data/private_key.pem` exist; if absent, add a dev script/steps to generate a local self-signed cert/key pair for testing (do not commit secrets).
  - [x] Validate that the `frontends/http3_server/init/wwatp` route in YAML matches the Request path used by tests (`/init/wwatp/`).
  - [x] Document all of the above verification steps and any deviations or environment-specific notes in `js_client_lib/README.md`.

- TLS provisioning for system_real_server.test.js
  - [x] Determine if server requires client certificate authentication (mTLS) or serves with server-only cert (curl requires `--cert`/`--key`).
  - [x] Decide client strategy for tests:
    - Short-term: Use `curl --http3` via child_process in the test to probe `https://127.0.0.1:12345/index.html` (with `-k` for self-signed) to validate handshake and content.
    - Long-term: Implement a Node-compatible HTTP/3/WebTransport adapter and thread cert/key/CA options via env (e.g., WWATP_CERT, WWATP_KEY, WWATP_CA, WWATP_INSECURE=1).
  - [x] If mTLS required, use the same self-signed pair for client and server in dev; paths are discoverable in tests.
  - [x] Add README notes on generating local certs and using them for e2e tests.
  - [x] Document the exact TLS setup required (paths, env vars, curl commands, adapter config) in `js_client_lib/README.md`.

- Transport adapter and connectivity experiments
  - [x] Survey Node options for HTTP/3/WebTransport:
        - node-webtransport (status/compat), quiche bindings, nghttp3 wrappers; Undici/fetch do not support HTTP/3.
  - [x] Prototype a minimal "connect + GET /index.html" using selected approach; meanwhile, shell out to curl as a bridge until a native adapter lands.
  - [x] Confirm that server responds to static asset fetch at `/index.html` using certs from YAML.
  - [x] Confirm WWATP route reachability at `/init/wwatp/` by POSTing WWATP chunk-framed bytes via curl and decoding the response.

- Test harness hardening for real server
  - [x] Update `system_real_server.test.js` to:
    - Spawn server with cwd=repo root; stream stdout; wait for UDP bind readiness; robust teardown on failures.
    - Pre-flight: when `WWATP_E2E=1`, run curl probe and assert response for `/index.html` using mTLS to validate TLS/QUIC.
    - Gate full protocol assertions behind feature flag until adapter exists; otherwise, skip with actionable message.
  - [ ] Parameterize port and cert paths via env to decouple from repository layout.
  - [x] Add timeouts and diagnostics for flakes (port in use, missing files).

- Operational checks and docs
  - [ ] Add a small Node script or Make/CMake test target to run `--check-only` and the smoke startup as CI preflight for e2e.
  - [ ] Verify `--help http3_server` and `--help backends` run without error and print usage (sanity for CLI wiring).
  - [x] Document run instructions in `js_client_lib/README.md` for the real-server e2e path, including cert generation, env vars, and how to enable `WWATP_E2E=1`.
  - [x] Document `WWATP_TRANSPORT=curl` usage and limitations (no streaming) in README.
  - [x] Add architecture note on heartbeats requiring the same QUIC connection; document why per-process curl heartbeats cannot flush WWATP responses and how LibcurlTransport solves it.

- Post-research follow-up
  - [ ] After completing the investigations and writing up results in `js_client_lib/README.md`, perform an audit to identify any missing JS coding tasks required for full integration (e.g., transport adapter gaps, response handling edge cases, env-driven configuration). Add those concrete tasks back into this TODO under the relevant sections with owners/priorities.


How to run

- Run mock system tests (default): from `js_client_lib`, run `npm test`.
- Run only system tests (mock):
  - By file: `npm test -- test/system/system_mock_transport.test.js`
  - By suite title (grep): `npm test -- --grep "System \\("` or `npm test -- --grep "System \\(mock transport\)"`
- Run real server e2e (after building server and enabling QUIC transport): set `WWATP_E2E=1` and run tests. Ensure `build/wwatp_server` exists and certificates under `test_instances/data` are present.
  - By file: `npm test -- test/system/system_real_server.test.js`
  - By suite title (grep): `npm test -- --grep "System \\(real server\)"`
  - With curl bridge: `WWATP_TRANSPORT=curl npm test -- --grep "System \\(real server\)"`
  - Run a single test by exact name with -t (append file for reliability):
    - `npm test -- -t "server starts, binds UDP port, and stops cleanly (smoke)" test/system/system_real_server.test.js`

- [ ] Browser-run tests (e.g., via Vitest + jsdom or Karma) for adapters; Node-only tests acceptable for heavier unit coverage.

## H. Docs and examples

- [ ] README in `js_client_lib` with quick-start, architecture diagram, notes on parity vs C++.
- [ ] Usage example: constructing backend, updater, and making a call.
- [ ] Notes on QUIC availability and using mock transport.
- [ ] Browser usage: bundler example (Vite), WebTransport capability detection, fallbacks.
 - [x] Document curl bridge adapter usage, env vars, and fallback behavior.
 - [x] Document libcurl HTTP/3 transport usage and same-connection heartbeat requirement in README.

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
Date: 2025-08-13

What we did this iteration
- Implemented Node-only curl bridge transport (`transport/curl_communication.js`) to reach the real WWATPService over HTTP/3 using curl.
- Wired the real-server system test to use the curl transport for a minimal getFullTree request and basic cache assertion, with graceful skips when curl/H3 or certs are unavailable.
- Updated README with curl bridge usage, env vars, and quick start; expanded TODO with staged integration plan and marked completed items.

Decisions
- Keep curl bridge non-streaming and Node-only; use it as an interim path for E2E until a native WebTransport/HTTP/3 adapter is ready.
- Maintain browser-first runtime code; expose Node-only adapters via separate modules and avoid bundling into browser builds.

Artifacts created/updated
- js_client_lib/transport/curl_communication.js: new adapter.
- js_client_lib/test/system/system_real_server.test.js: switched the final test to exercise curl transport for minimal E2E.
- js_client_lib/README.md: added "Curl bridge transport" section and instructions.
- js_client_lib/TODO.md: added and checked tasks for curl bridge and tests.

Update (later in session)
- Implemented a Node-only LibcurlTransport that reuses a single QUIC connection and allows server-required heartbeats to arrive on subsequent streams.
- Switched real-server WWATP tests to prefer LibcurlTransport when available; kept curl CLI for static asset probe.
- Added README architecture notes explaining same-connection heartbeat requirement.

Status updates
- Real-server E2E path can run a minimal request over curl when enabled; suite continues to gate on WWATP_E2E and skips if curl/H3 is unavailable.

Notes / deferrals
- Streaming/server-push not supported in curl bridge (by design).
- Still pending: uniform handling for empty/signal-only responses; journal mutable page tree edge-case; optional server sync flags helpers.

Open items (next steps)
- Evaluate native WebTransport/HTTP/3 adapter for Node/browser and gradually replace curl bridge where feasible.
- Close remaining response-handling edge cases; consider parameterizing real-server test ports and paths via env.

Additional updates (late):
- Refactored real-server test to use backend_testbed sequence (add animals, add notes, then logical checks) and renamed it to "libcurl testBackendLogically" to improve filtering reliability.
- Added two new real-server tests using a single LibcurlTransport connection and two clients to the same URL:
  - "test roundtrip": seed via client A; verify full logical state via client B with BackendTestbed.
  - "test deleteElephant": seed via client A; delete 'elephant' via client A; sync client B and verify deletion with checkMultipleDeletedNode.
- Verified both new tests execute and pass when WWATP_E2E=1 and node-libcurl (HTTP/3) is available; tests skip gracefully otherwise.

## L. Real-server integration plan (staged)

Stage 1 — Curl bridge (Node-only)
- Implement `transport/curl_communication.js` and select with `WWATP_TRANSPORT=curl`.
- Scope: request/response flows that complete in a single body (RESPONSE_FINAL). Journaling is periodic short-lived requests.
- Update `system_real_server.test.js` to exercise one happy-path backend call over the curl transport.

Stage 2 — Native HTTP/3/WebTransport adapter
- Evaluate `node-webtransport` or bindings to `ngtcp2`/`quiche` for streaming support.
- Replace curl in tests where available; keep curl as CI fallback.

Stage 3 — WebSocket sidecar bridge (optional)
- Provide a dev sidecar that proxies WS<->HTTP/3 preserving WWATP framing for browser demos.
- Add `websocket_communication.js` and document setup.

---

## How to run tests (npm)

From the JS library folder:

1) Run the full suite
  - change directory to `js_client_lib`
  - run: `npm test`

2) Run a focused test
  - `npm test -- -t http3_client_updater`

If invoking from repo root, you can also run: `cd js_client_lib && npm test`


## M. Conversation summary (latest)
Date: 2025-08-13

Overview
- Goal: reach the C++ WWATPService from JS, initially via a curl bridge, then a native HTTP/3 path; ensure message encoding matches C++ and handle the server’s heartbeat flush behavior.

Key steps
- Implemented curl-based transport that shells out to curl --http3 with mTLS; integrated into system tests (config, UDP bind, static probe, minimal WWATP flows).
- Discovered server behavior: WWATP responses may flush only after heartbeats on the same QUIC connection; appending an inline heartbeat on the same stream didn’t unblock.
- Implemented C++-parity chunk encoders/decoders in JS and aligned boolean responses to label-encoded "true"/"false".
- Verified that separate curl processes don’t share a QUIC connection, so heartbeats sent that way can’t flush.
- Built a Node-only LibcurlTransport that reuses one HTTP/3 connection and opens new streams for follow-up heartbeats; switched WWATP tests to use it.
- Kept curl CLI for static asset/health checks. Real-server tests pass end-to-end with LibcurlTransport.

Updates from this round
- Fixed the real-server parity test to actually use the backend_testbed sequence (add animals + add notes + logical checks) and renamed it to "libcurl testBackendLogically" to avoid grep issues with "+".
- Added "test roundtrip" using two clients to validate round-trip state sync via getFullTree on client B.
- Added "test deleteElephant" to validate delete propagation; asserts via checkMultipleDeletedNode on client B.
- Confirmed these tests run and pass when node-libcurl HTTP/3 is available; otherwise they skip as designed under the E2E gate.

What’s next
- Move toward WebTransport for browsers; keep libcurl for Node/CI until WebTransport is ready. Parameterize ports/paths and close remaining response edge cases.


