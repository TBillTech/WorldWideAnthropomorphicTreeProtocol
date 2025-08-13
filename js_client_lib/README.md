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

Run readiness tests
- From `js_client_lib`:
	- Enable Group B tests: set `WWATP_E2E=1`.
	- Run: `npm test -- -t "System (real server)"`.
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
	- `WWATP_E2E=1 WWATP_TRANSPORT=curl npm test -- -t "System (real server)"`
3) If curl lacks HTTP/3, install a build with HTTP/3 support or skip this path.

Expected outcome
- The test will run a minimal getFullTree request over the curl bridge and assert that the protocol path works end-to-end. The returned vector may be empty depending on server content; the goal is transport verification.

## Mock transport tests

- Run all tests: `npm test` from `js_client_lib`.
- The mock server exercises the protocol framing over an in-memory transport and validates backend behaviors without QUIC.

