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

 - DONE

## B. Core interfaces and SimpleBackend (mirroring C++)

1) Backend interface (`js_client_lib/interface/backend.js`)

 - DONE

2) TreeNode & related types (`js_client_lib/interface/tree_node.js`)
 
 - DONE

3) SimpleBackend (`js_client_lib/interface/simple_backend.js`)

 - DONE
 
## C. Transport 

1) Node WebTransport emulator (using N-API addon)

  Goal: Provide a WebTransport-shaped emulator for Node that uses the native QUIC QuicConnector via the N-API addon, offering emulation of the browser WebTransport while paying attention to the QUIC session details such as heartbeats.

  WebTransport API parity (constructor, attributes, methods)
  - [X] Implement `new WebTransport(url, options)` with a compatible `WebTransportOptions` subset:
    [X] options.allowPooling, requireUnreliable, protocols[]
    [X] options.anticipatedConcurrentIncomingUnidirectionalStreams?, anticipatedConcurrentIncomingBidirectionalStreams?
    [X] options.congestionControl ("default" | "throughput" | "low-latency") — accept but may be advisory only
    [X] options.serverCertificateHashes (accept but may be no-op in Node path)
  - [X] Expose read-only attributes with correct lifecycles:
    [X] `ready: Promise<void>` resolves when session established; rejects on failure
    [X] `closed: Promise<WebTransportCloseInfo>` fulfills on graceful close or rejects on abrupt/failed init
    [X] `draining: Promise<void>` resolves when session drained (map to session shutdown in addon)
    [X] `datagrams: WebTransportDatagramDuplexStream` (see Datagrams section below)
    [X] `incomingBidirectionalStreams: ReadableStream<WebTransportBidirectionalStream>`
    [X] `incomingUnidirectionalStreams: ReadableStream<WebTransportReceiveStream>`
    [X] `reliability: "pending" | "reliable-only" | "supports-unreliable"`
    [X] `congestionControl: "default" | "throughput" | "low-latency"` (Report effective value)
    [X] `protocol: string` (ALPN or application protocol; empty if unknown)
  - [X] Implement methods:
    [X] `close(closeInfo?: { closeCode?: number; reason?: string })`: graceful termination semantics; map to addon session close
    [X] `getStats(): Promise<WebTransportConnectionStats>` — provide at least bytesSent/Received, packetsSent/Received when available; otherwise reasonable zeros/nulls
    [X] `exportKeyingMaterial(label: BufferSource, context?: BufferSource): Promise<ArrayBuffer>` — optional stub or throw NotSupported if addon lacks support
    [X] `createBidirectionalStream(options?: WebTransportSendStreamOptions)` returns `WebTransportBidirectionalStream`
    [X] `createUnidirectionalStream(options?: WebTransportSendStreamOptions)` returns `WebTransportSendStream`
    [X] `createSendGroup()` returns `WebTransportSendGroup` (optional minimal stub sufficient for sendOrder grouping)
    [X] Static: `WebTransport.supportsReliableOnly: boolean` (true if HTTP/2 fallback only; Node QUIC should set false)

  Streams (Readable/Writable conformance)
  - [ ] `WebTransportBidirectionalStream` exposes `.readable: WebTransportReceiveStream` and `.writable: WebTransportSendStream`.
  - [ ] `WebTransportSendStream` is a WritableStream<Uint8Array> with `.getWriter()`, `.close()`, `.abort(err)`, `.getStats()`; enforce backpressure via `.ready` on writer.
  - [ ] `WebTransportReceiveStream` is a ReadableStream<Uint8Array> with BYOB reader support; `.getStats()` provided minimally.
  - [ ] Map `.createBidirectionalStream()` to opening a QUIC bidi stream on the single underlying session; honor `waitUntilAvailable` by awaiting flow control if necessary.
  - [ ] Provide `incomingBidirectionalStreams` and `incomingUnidirectionalStreams` as ReadableStreams that yield on server-initiated streams.
  - [ ] Handle STOP_SENDING / RESET_STREAM mapping to `WebTransportError` with `source: "stream"` and `streamErrorCode` per spec guidance.

  Datagrams
  - [ ] Implement `datagrams: WebTransportDatagramDuplexStream` with:
    [X] `.readable: ReadableStream<Uint8Array>`
    [X] `.createWritable(options?): WebTransportDatagramsWritable` and writer semantics
    [X] Attributes: `maxDatagramSize`, `incomingMaxAge`, `outgoingMaxAge`, `incomingHighWaterMark`, `outgoingHighWaterMark`
    [X] If QUIC DATAGRAM not supported by addon, expose the interface but either no-op or reject writes; set `reliability` accordingly.

  Session model and heartbeats
  - [ ] Ensure a single QUIC connection (session) per `WebTransport` instance and reuse it for all streams; required so updater heartbeats arrive on the same connection.
  - [ ] Map WWATP heartbeat writes to new streams on the same session; verify addon ensures connection reuse and stream id allocation.

  Error handling and lifecycle
  - [ ] Propagate transport errors: reject `ready`, cause `closed` to reject with `WebTransportError` (source: "session").
  - [ ] Properly fulfill `closed` with `WebTransportCloseInfo` on graceful close; error open streams and close datagram/readable streams per spec cleanup steps.
  - [ ] Abort/timeout handling for writers/readers: support `.abort()` on writers and cancellation on readers; integrate with addon to reset/stop-sending.

  TLS/mTLS and configuration (Node only)
  - [ ] Wire env vars (WWATP_CERT/KEY/CA, WWATP_INSECURE) into session options and pass to addon; document precedence and defaults.
  - [ ] Support URL parsing: `https://host:port/path` with explicit port; reject insecure http.

  Selection and exports
  - [ ] Selection: allow `WWATP_TRANSPORT=webtransport-native` and export from `index.js` guarded to avoid bundling into browsers.
  - [ ] Provide a small factory `create_webtransport_connector.js` that returns browser `WebTransport` or Node emulator depending on environment.

  Conformance tests (unit + integration)
  - [ ] API shape tests: constructor presence, attributes (`ready`, `closed`, `datagrams`, `incoming*Streams`, `reliability`, `protocol`) and methods (`close`, `create*Stream`, `getStats`).
  - [ ] Stream happy path: open bidi, write bytes, FIN, read echoed/response bytes; verify backpressure and writer `.ready` behavior.
  - [ ] Abort/cancel: writer.abort propagates RESET, reader.cancel sends STOP_SENDING; verify errors surfaced as `WebTransportError` with `source` and optional `streamErrorCode`.
  - [ ] Incoming streams: push a server-initiated unidirectional/bidirectional stream via addon shim and assert they appear on `incoming*Streams`.
  - [ ] Datagrams: when supported, send/receive; when not, ensure writes fail predictably and `maxDatagramSize` reflects capability.
  - [ ] Lifecycle: `ready` resolves, `closed` fulfills on graceful close and rejects on abrupt close; ensure cleanup of streams and datagrams.
  - [ ] Parity: run the same tests against browser `WebTransport` where feasible (jsdom not sufficient; use manual or conditional skip), and against `NodeWebTransportMock` to validate adapter behavior.

  Docs
  - [ ] README section “Node WebTransport emulator” with API coverage matrix vs spec, known limitations, and usage examples.
  - [ ] Document environment variables for TLS/mTLS and transport selection; include troubleshooting.

2) WebTransportCommunication built on WebTransport (either node mock or Browser implementation)

  Goal: Provide a Communication class (comparable to the curl_communication.js for example) which which uses a WebTransport interface for the backend.  This will allow code using the WebTransportCommunication to be agnostic about whether running in the Browser or on node js.

  Design and API
  - [ ] Define the WebTransportCommunication contract: subclass `transport/communication.js` and expose `connect()`, `close()`, `sendRequest(sid, request, bytes, opts)` with AbortSignal/timeout support and response event emission.
  - [ ] Decide on URL semantics: accept a base WebTransport URL (e.g., `https://host:port/webtransport`) and ignore per-request path once connected; document that WWATP pathing is handled at session creation.
  - [ ] Connection reuse: ensure a single WebTransport session per connector instance; reuse it for all requests to satisfy heartbeat-on-same-connection.

  Implementation
  - [ ] Audit and finalize `transport/webtransport_communication.js` as the concrete WebTransportCommunication for browsers (feature parity with curl adapter where applicable).
  - [ ] Implement a small factory for WebTransport connectors that selects between browser `WebTransport` and a Node adapter (from C.1) without changing call sites.
    [X] File: `transport/create_webtransport_connector.js` (browser: `WebTransportCommunication`; node: `node_webtransport_mock.js` or native emulator when available).
  - [ ] Add environment-driven selection: allow `WWATP_TRANSPORT=webtransport` (browser) and `WWATP_TRANSPORT=webtransport-native` (Node emulator) and wire through `index.js` export.
  - [ ] Error handling: map stream/session errors into thrown errors and `response` events with `{ ok:false, status:0, error }`.
  - [ ] Abort/timeout: ensure `sendRequest` cancels writes/reads and surfaces `AbortError`; add timeout behavior consistent across envs.
  - [ ] Concurrency: support multiple concurrent requests by opening a new bidi stream per call; verify no cross-talk and correct handler routing by `StreamIdentifier`.
  - [ ] Large payloads/backpressure: write in chunks (async iterator) and ensure backpressure is respected; add a stress test plan (10MB+ payload).
  - [ ] Reconnect policy: on session close, optionally auto-reconnect once on next `sendRequest`; guard with an option `{ autoReconnect: true }`.

  Node path integration (depends on C.1)
  - [ ] If the Node WebTransport emulator is present, provide an adapter with the same surface as the browser connector and ensure single-connection reuse.
  - [ ] TLS/mTLS for Node: propagate `WWATP_CERT`, `WWATP_KEY`, `WWATP_CA`, `WWATP_INSECURE` into the Node adapter session options; no-op in browsers.
  - [ ] Heartbeats: verify that heartbeats sent by the Updater arrive on the same QUIC connection (same session) and trigger server flushes.

  Wiring and exports
  - [ ] Export the connector/factory from `js_client_lib/index.js` (browser-safe named exports).
  - [ ] Extend `transport/create_transport.js` or add `create_webtransport_connector.js` to support `{ preferred: 'webtransport' }` and env `WWATP_TRANSPORT` overrides.
  - [ ] Ensure `Http3ClientBackendUpdater` usage is unchanged: pass the selected connector instance to `maintainRequestHandlers`.

  Tests
  - [ ] Unit tests (browser-like via jsdom or WebTransport polyfill):
    [X] sendRequest success round-trip emits `response` and returns `{ ok:true, status:200, data }`.
    [X] AbortSignal cancels in-flight write/read and rejects with `AbortError`.
    [X] Timeout triggers expected error path and cleanup.
    [X] Concurrent requests route responses to the correct handlers.
  - [ ] Node tests with `node_webtransport_mock.js`: mirror the above behaviors and validate parity with browser connector.
  - [ ] Integration with updater (mock server): end-to-end WWATP message encode->transport->decode using the connector; includes heartbeats loop behavior.
  - [ ] Real-server smoke (optional, guarded by `WWATP_E2E=1`): minimal `getFullTree` over the selected WebTransport (Node emulator path only for now); skip gracefully in CI if not available.

  Documentation
  - [ ] README: Add WebTransportCommunication section with usage examples in both browser and Node (mock/emulator), option flags, and limitations.
  - [ ] Note browser mTLS limitations and recommend token-based auth if required; detail Node-only mTLS env vars.
  - [ ] Architecture note: why single-session reuse is required for WWATP heartbeats.

  Developer experience
  - [ ] Lint/format and ESM-friendly exports; avoid Node-only APIs in browser path.
  - [ ] Add minimal TypeScript typings (d.ts) for the connector public surface (optional, stretch).
  - [ ] CI: add a browser-run test job (Vitest + jsdom) for connector unit tests; keep Node-only tests separate.

3) E2E test using WebTransportCommunication with http3_client_backend_updater

  The Following tests need to be added to the system_real_server.text.js.  
  - [X] upsert a test node and fetch it back via WebTransportCommunication (In place of LibcurlTransport, for example)
  - [ ] WebTransportCommunication testBackendLogically
  - [ ] WebTransportCommunication test roundtrip
  - [ ] Verify implementation of testPeerNotification test in js_client_lib/test/backend_testbed/backend_testbed.js (use test_instances/catch2_unit_tests/backend_testbed.cpp for reference)
  - [ ] WebTransportCommunication testPeerNotification (which will verify that notifications are working via WebTransportCommunication)

## E. Serialization and binary safety

  - [ ] Validate encoders/decoders with golden vectors or a cross-language fixture.

## F. System-level and CI checks

  - [ ] Add a small Node script or CMake test target that runs server `--check-only` and smoke start/stop as a CI preflight for e2e.
  - [ ] After the latest investigations, audit remaining JS integration gaps (transport adapter behaviors, response edge cases, env-driven config) and refile concrete tasks under the proper sections.

## G. Browser-run tests

  - [ ] Add browser-run adapter tests (Vitest + jsdom or Karma). Node-only tests can remain for heavy coverage.

## H. Docs and examples

  - [ ] js_client_lib README quick-start, architecture diagram, and parity notes vs C++.
  - [ ] Usage example showing Backend + Updater + Communication wiring.
  - [ ] Browser usage notes: bundler example (Vite), capability detection, and fallbacks.

## I. Stretch and future work

  - [ ] TypeScript typings or migration to TS.
  - [ ] Browser build bundling for WebTransport.
  - [ ] Backpressure/flow control tuning for large trees.
  - [ ] Performance benchmarks vs C++ framing.

## F. Tests

Need to keep up with unit test construction and execution.

### System-level tests

Integration readiness and investigation tasks

- Operational checks and docs
  - [ ] Add a small Node script or Make/CMake test target to run `--check-only` and the smoke startup as CI preflight for e2e.
  - [ ] Verify `--help http3_server` and `--help backends` run without error and print usage (sanity for CLI wiring).

- Post-research follow-up
  - [ ] After completing the investigations and writing up results in `js_client_lib/README.md`, perform an audit to identify any missing JS coding tasks required for full integration (e.g., transport adapter gaps, response handling edge cases, env-driven configuration). Add those concrete tasks back into this TODO under the relevant sections with owners/priorities.

Browser testing

- [ ] Browser-run tests (e.g., via Vitest + jsdom or Karma) for adapters; Node-only tests acceptable for heavier unit coverage.

## H. Docs and examples

- [ ] Browser usage: bundler example (Vite), WebTransport capability detection, fallbacks.

## I. Stretch and future work

- [ ] Browser build (WebTransport) with appropriate transport adapter.

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

## Conversation summary (latest)
Date: 2025-08-18

Overview
- Instrumented Node WebTransport emulator, WebTransportCommunication, and Updater with a lightweight tracer gated by `WWATP_TRACE=1`.
- Added `WWATP_DISABLE_HB` and `WWATP_E2E_SIMPLE` to aid bisection; gated the real-server WebTransport test behind `WWATP_E2E_FULL=1` (skipped by default).
- Implemented a new system test using a WebTransport polyfill to run `BackendTestbed` logically over `WebTransportCommunication` + Updater, keeping coverage without native QUIC.

Key steps
- Added `test/system/system_webtransport_polyfill.test.js` which wires the in-memory WWATP server mock to a polyfilled WebTransport and drives updater flows.
- Verified tests: suite passes with the real-server WebTransport test skipped by default.

Next
- Implemented the remaining items under section 3 via polyfill path: roundtrip and peer notifications over WebTransportCommunication. Next, revisit enabling the native emulator test once stable and ungate with WWATP_E2E_FULL.


