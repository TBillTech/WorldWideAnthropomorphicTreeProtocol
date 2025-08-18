# JS Client Library

This README documents how to run the JS client library’s tests against both a mock transport and the real C++ WWATP server.

## Real server e2e

You can run a system-level smoke test against the C++ WWATP server binary to validate config, startup, and HTTP/3 reachability.

Prereqs
- Build the server: from repo root, build the CMake project so `build/wwatp_server` exists.
- OpenSSL installed (optional, for generating self-signed certs in dev).
- curl with HTTP/3 support (for the probe step).

Config and files
- Config YAML: `js_client_lib/test/resources/wwatp_server_config.yaml`.
- Referenced files are under `test_instances/data`; logs go to `test_instances/sandbox`.
- Required files:
	- `test_instances/data/cert.pem` and `test_instances/data/private_key.pem` (self-signed OK for local).
	- `test_instances/data/libtest_index.html` and `test_instances/data/randombytes.bin` (present in repo).

YAML CLI support and check-only
- The server now accepts a YAML file path directly. If you pass a path ending with `.yaml`, the server reads it and initializes from the YAML string.
- Use `--check-only` to validate configuration without starting listeners; exit code 0 means success.

Generating dev TLS certs
- Set `WWATP_GEN_CERTS=1` when running tests to auto-generate self-signed certs for localhost if missing.
- The test will create `test_instances/data/cert.pem` and `test_instances/data/private_key.pem` using OpenSSL.

How to run tests

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

Run readiness tests
- From `js_client_lib`:
	- Enable Group B tests: set `WWATP_E2E=1`.
	- Run just the real-server suite (Vitest grep filters suites/tests):
	  - `npm test -- --grep "System \\("` or more specific `npm test -- --grep "System \\("` with the full suite text `System \(real server\) – integration readiness`.
	  - Alternatively, run by file: `npm test -- test/system/system_real_server.test.js`
	- The suite performs:
		1) `--check-only` config validation and expects the message "Configuration validation completed successfully".
		2) Smoke start/stop: launch server, wait for UDP QUIC port bind, then stop.
		3) HTTP/3 curl probe for `/index.html` using mutual TLS (see below).
		4) Placeholder client flow until a Node/browser HTTP/3/WebTransport adapter is wired.


Mutual TLS for curl probe
- Current server setup expects a client certificate. The tests invoke curl with mTLS flags:
	- `--http3 -k --cert test_instances/data/cert.pem --key test_instances/data/private_key.pem`
- The test includes retries and will skip this step if curl lacks HTTP/3 support.

Readiness detection
- Tests wait for the UDP port from YAML to be bound (not just a log line) before proceeding, improving reliability.

Environment variables
- `WWATP_E2E=1`: enable real-server tests.
- `WWATP_GEN_CERTS=1`: auto-generate self-signed cert/key if missing.

Troubleshooting
- If port 12345 is in use, edit the YAML; env overrides are planned.
- Ensure `build/wwatp_server` exists; otherwise run the CMake build first.
- Prefer forward slashes in YAML paths; tests run with cwd at repo root.

### Curl bridge transport (interim option)

Until a native HTTP/3/WebTransport adapter is available for Node/browser, you can run real-server flows through a small Node-only adapter that shells out to curl.

Status
- Implemented as `transport/curl_communication.js`. It implements the Communication interface by executing `curl --http3` per request and piping WWATP-framed bytes.

How tests will select it
- Set `WWATP_TRANSPORT=curl` along with `WWATP_E2E=1`.
- The system test will instantiate the updater with the curl transport and perform a minimal request to `/init/wwatp/`.

TLS/mTLS options (from env)
- `WWATP_CERT`, `WWATP_KEY`, `WWATP_CA` for client cert/key/CA paths.
- `WWATP_INSECURE=1` to add `-k` for curl during local testing.

Limitations
- Non-streaming only: responses are treated as complete bodies (RESPONSE_FINAL). Server push/streaming semantics are not supported in this bridge.
- Node-only. This module is excluded from browser bundles.

Quick start
1) Build server and ensure certs exist (or set `WWATP_GEN_CERTS=1`).
2) From `js_client_lib`:
	- `WWATP_E2E=1 WWATP_TRANSPORT=curl npm test -- --grep "System \\("` (or target the file: `npm test -- test/system/system_real_server.test.js`)
3) If curl lacks HTTP/3, install a build with HTTP/3 support or skip this path.

### Important: place real-server tests in one suite (sequential)

- To avoid port contention in IDEs (e.g., VS Code) that run tests in parallel, all tests that talk to the real WWATP service should live in `test/system/system_real_server.test.js`.
- That file’s suite is explicitly marked sequential, so its sub-tests run in series and reuse the same port without conflicts.
- Don’t scatter real-server tests across other files; consolidate them into this suite to benefit from sequential execution.

Filtering specific tests (Vitest)
- Filter by suite/test title: `npm test -- --grep "System \\("` (matches suite names too).
- Filter by file: `npm test -- test/system/system_mock_transport.test.js`.
- Run a single test by exact name with -t (matches test cases, not suite names):
	- Example: `npm test -- -t "server starts, binds UDP port, and stops cleanly (smoke)" test/system/system_real_server.test.js`
	- Note: `-t "System (real server)"` won’t match because that’s a suite title; use `--grep` for suites.



Expected outcome
- The test will run a minimal getFullTree request over the curl bridge and assert that the protocol path works end-to-end. The returned vector may be empty depending on server content; the goal is transport verification.

## Architecture notes: heartbeats and QUIC connection reuse

- The WWATP server may buffer responses and only release them after it receives a follow-up heartbeat message associated with the same logical request id.
- Critically, that heartbeat must arrive on the same QUIC connection. A later stream on the same connection is acceptable; a separate QUIC connection is not.
- Implication for clients:
	- Per-process curl CLI calls generally establish a fresh QUIC connection each time. Heartbeats sent via separate curl processes won’t flush the pending response.
	- Appending a heartbeat in the same request stream may also not trigger a flush if the server expects the heartbeat on a subsequent stream.
	- A client that can reuse one QUIC connection (e.g., libcurl Multi or WebTransport session) is required for reliable WWATP flows.

### Libcurl HTTP/3 transport (single-connection reuse)

If you have libcurl with HTTP/3 available, you can use a Node-only transport that reuses a single QUIC connection and opens new streams for follow-ups (e.g., heartbeats).

Status
- Implemented as `transport/libcurl_transport.js` (Node-only). Exported as `LibcurlTransport` from `index.js`.

Install
- Add node-libcurl (prebuilt binaries exist for common platforms):
	- `npm install node-libcurl` (or it will be installed automatically as an optional dependency on fresh installs)
- Ensure your system curl/libcurl supports HTTP/3 (nghttp3/ngtcp2). `curl -V` should list `HTTP3` under Features.

TLS/mTLS
- Same env vars as the curl bridge: `WWATP_CERT`, `WWATP_KEY`, `WWATP_CA`, `WWATP_INSECURE=1`.

Usage (tests/dev)
- Import dynamically to avoid bundling in browser:
	- `const { LibcurlTransport } = await import('../index.js');`
	- Instantiate and pass to `Http3ClientBackendUpdater.maintainRequestHandlers()` similar to the curl bridge.

Notes
- This transport keeps a single HTTP/3 connection and is better aligned with server behavior that requires heartbeats on the same QUIC connection.

## Node C Gyp QuicConnector Module

A native Node module exposes the C facade defined in `common/transport/include/quic_connector_c.h` to provide an HTTP/3/QUIC client used by tests and Node-only adapters.

Start here:
- Native addon README: `js_client_lib/native/README.md` (build and runtime details)
- Loader/transport: `transport/native_quic.js` (tries the addon first, falls back to FFI if configured)

Build prerequisites
- Ensure the shared library `libwwatp_quic.*` is produced by the CMake build (`BUILD_WWATP_QUIC_C=ON`, default). Output is under `build/`.
- From `js_client_lib/`, run the addon build: `npm run build:native`.

Usage (Node-only)
- Import dynamically where needed:
	- `const { NativeQuic } = await import('../index.js');`
	- Create a session with TLS/mTLS options, open a bidi stream (e.g., `/init/wwatp/`), write WWATP-framed bytes, read, then close.
- The transport keeps a single QUIC connection to satisfy heartbeat-on-same-connection semantics.

Notes
- Prefer the N-API addon path for performance and proper async I/O; the FFI path is primarily for experimentation.
- See the native README for environment variables (e.g., `WWATP_QUIC_SO`) and troubleshooting.

## Mock transport tests

- Run all tests: `npm test` from `js_client_lib`.
- The mock server exercises the protocol framing over an in-memory transport and validates backend behaviors without QUIC.

## Node WebTransport emulator (Node-only)

A WebTransport-shaped emulator is available for Node to enable single-connection QUIC semantics using the native N-API addon. It reuses one QUIC session and opens a bidi stream per request, matching WWATP heartbeat requirements.

- File: `transport/node_webtransport_emulator.js` (exported as `NodeWebTransportEmulator`)
- Factory: `transport/create_webtransport_connector.js` (exported as `createWebTransportConnector`)
- Usage:
  - `const { createWebTransportConnector } = await import('../index.js');`
  - `const wt = createWebTransportConnector('https://127.0.0.1:12345/init/wwatp/', { insecure: true });`
  - `await wt.ready; const bidi = await wt.createBidirectionalStream();`
- TLS/mTLS env:
  - `WWATP_CERT`, `WWATP_KEY`, `WWATP_CA`, `WWATP_INSECURE=1`
- Notes:
  - Datagrams are not supported yet; methods are stubs.
  - If the native addon is not built, creation will throw or `ready` will reject.

## Browser WebTransport

Feature detection and selection
- Use `createTransportForUrl(url)` from `index.js` to pick the best browser transport at runtime.
	- If `WebTransport` is available in a secure context, it returns a `WebTransportCommunication`.
	- Otherwise, it falls back to `FetchCommunication` (non-streaming) to keep basic flows working.
- You can force a choice by passing `{ preferred: 'webtransport' | 'fetch' }`.

Abort/timeout
- `WebTransportCommunication.sendRequest(sid, request, data, { timeoutMs, signal })` supports optional timeouts and `AbortSignal` to cancel in-flight requests.

Chunk framing and WWATP
- The adapter writes exactly the bytes you pass (pre-framed WWATP chunks) and assembles the full response payload before emitting a `response` event.
- For truly streaming use-cases, extend the adapter to surface progressive reads via an additional callback or a readable stream.

Troubleshooting
- WebTransport requires a secure context (https or localhost) and a browser with the API enabled.
- If `WebTransport` is undefined, ensure you're on Chrome/Edge recent versions and not in an insecure http page.
- Server must advertise WebTransport/HTTP/3 and support datagrams/bidi streams; otherwise the transport will fail to establish.


## Native QUIC (FFI) POC

A minimal Node-only loader is provided to experiment with a native HTTP/3 client via the C facade:

- Loader: `transport/native_quic.js` (exported as `NativeQuic` and `tryLoadNativeQuic`)
- Library: built as `libwwatp_quic.*` under `build/` when `BUILD_WWATP_QUIC_C=ON` (default)
- Usage:
	- `const { NativeQuic } = await import('../index.js');`
	- `const nq = NativeQuic(); // null if ffi modules or lib missing`
	- `const sess = nq.createSession({ url: 'https://127.0.0.1:12345' });`
	- `const st = nq.openBidiStream(sess, '/init/wwatp/');`
	- `nq.write(st, new Uint8Array([1,2,3]), true);`
	- `const out = nq.read(st);`
	- `nq.closeStream(st); nq.closeSession(sess);`

Notes
- Current C implementation is a stub that echoes bytes. Replace with QuicConnector-backed logic to perform real HTTP/3.
- The test `test/native_quic.test.js` auto-skips when prerequisites are not met.

