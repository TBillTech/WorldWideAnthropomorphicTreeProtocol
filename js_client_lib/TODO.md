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
 
## C. Transport – Node WebTransport mock (native QUIC)

  Goal: Provide a WebTransport-shaped adapter for Node that uses the native QUIC QuicConnector via the N-API addon, offering parity with the browser adapter while reusing a single QUIC session for heartbeats.

  - [ ] Design API surface matching the browser WebTransport adapter: connect(), close(), sendRequest(streamId, bytes, opts), session reuse.
  - [ ] Implement against native addon (`js_client_lib/native`), fallback to FFI loader in `transport/native_quic.js` only if addon is unavailable.
  - [ ] Map WebTransport bidi streams to QuicConnector streams; ensure same-connection requirement for heartbeats.
  - [ ] TLS/mTLS: wire env vars (WWATP_CERT/KEY/CA, WWATP_INSECURE) into session options.
  - [ ] Error handling: timeouts, AbortSignal support, orderly close, and cleanup on transport errors.
  - [ ] Tests: unit conformance and system parity vs mock transport and LibcurlTransport.
  - [ ] Selection: allow `WWATP_TRANSPORT=webtransport-native` and export from `index.js`.
  - [ ] Docs: README “Node C Gyp QuicConnector Module” and adapter usage example.

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
Date: 2025-08-14

Overview
- Goal: stabilize native QUIC integration for Node by replacing fragile FFI with a robust N-API addon, keep browser-first design elsewhere, and ensure tests pass end-to-end.

Key steps
- Implemented an N-API addon (`js_client_lib/native`) wrapping the C facade in `quic_connector_c.h`; linked against `build/libwwatp_quic.so` with correct rpath.
- Updated loader `transport/native_quic.js` to prefer the addon exclusively and removed all ffi-napi/ref* code and dependencies.
- Fixed addon path resolution and binding.gyp include/-L/rpath settings; validated exports and runtime loading.
- Updated tests (`test/ffi_sanity.test.js`, `test/native_quic.test.js`) to use the addon only; added TLS options (cert/key or insecure flag) for session creation; tests pass when addon is built.
- Retained existing LibcurlTransport real-server tests; addon covers native QUIC session/stream operations and is the foundation for future Node QUIC transport.

Decisions
- Standardize on N-API addon for Node integration; drop ffi-napi entirely.
- Keep browser-facing code free of Node-only APIs; continue WebTransport work in parallel.

Artifacts updated
- js_client_lib/transport/native_quic.js: addon-only loader.
- js_client_lib/native/binding.gyp and src/addon.cc: addon build and exports.
- js_client_lib/test/ffi_sanity.test.js and native_quic.test.js: addon-based tests with graceful skips if addon not built.
- js_client_lib/package.json: removed ffi-napi/ref* deps; ensured node-addon-api/node-gyp present.


