// Node-only transport adapter that shells out to `curl --http3` per request.
// This is an interim bridge to reach a real WWATPService over HTTP/3 until
// a native Node/WebTransport adapter is available. Not bundled for browsers.
//
// Important server quirk and workaround (QUIC FIN interaction):
// - The server may return no body to the initial request carrying a WWATP message,
//   and sometimes will close that stream right away. The server only releases the
//   accumulated response when it receives a small follow-up message (a heartbeat)
//   on a subsequent client stream with the same logical request id. See the C++
//   maintainRequestHandlers() which injects a payload_chunk_header with
//   SIGNAL_HEARTBEAT when both request and response are empty.
// - Heartbeats are orchestrated by the Http3ClientBackendUpdater in JS, not here.
//   This module focuses on spawning curl, marshalling bytes, and honoring timeouts.
//
// Usage notes
// - Selected via env/explicit import in tests (do not export from index.js for browser safety).
// - TLS env vars:
//   WWATP_CERT, WWATP_KEY, WWATP_CA, WWATP_INSECURE=1
// - Non-streaming: we treat each curl call as a "burst"; heartbeats are separate bursts.

import Communication from './communication.js';
import { spawn } from 'child_process';
// no imports from helpers needed here; this adapter just ships bytes

function envFlag(name) {
	return typeof process !== 'undefined' && process.env && process.env[name];
}

function buildCurlArgs(url, method, hasBody) {
	// If we have a body but the method is GET, switch to POST for interoperability.
	const effectiveMethod = hasBody && (!method || String(method).toUpperCase() === 'GET') ? 'POST' : (method || 'GET');
	const args = ['--http3', '-sS', '-i', '-X', effectiveMethod, url, '-H', 'content-type: application/octet-stream', '-H', 'accept: application/octet-stream'];
	const cert = envFlag('WWATP_CERT');
	const key = envFlag('WWATP_KEY');
	const ca = envFlag('WWATP_CA');
	const insecure = envFlag('WWATP_INSECURE');
	const verbose = envFlag('WWATP_CURL_VERBOSE');
	if (cert) args.push('--cert', cert);
	if (key) args.push('--key', key);
	if (ca) args.push('--cacert', ca);
	if (insecure) args.push('-k');
	if (verbose) args.push('-v');
	if (hasBody) args.push('--data-binary', '@-');
	return args;
}

// Parse `curl -i` output: headers+CRLFCRLF+body. Returns { status, headersText, bodyBytes }.
function parseCurlHttpOutput(stdoutBytes) {
	// Look for CRLFCRLF or LF LF separators
	const u8 = new Uint8Array(stdoutBytes);
	let sepIndex = -1;
	// Search CRLFCRLF
	for (let i = 0; i + 3 < u8.length; i++) {
		if (u8[i] === 13 && u8[i + 1] === 10 && u8[i + 2] === 13 && u8[i + 3] === 10) {
			sepIndex = i + 4;
			break;
		}
	}
	if (sepIndex === -1) {
		// Search LFLF
		for (let i = 0; i + 1 < u8.length; i++) {
			if (u8[i] === 10 && u8[i + 1] === 10) {
				sepIndex = i + 2;
				break;
			}
		}
	}
	let headersText = '';
	let body = u8;
	let status = 0;
	if (sepIndex !== -1) {
		const headerBytes = u8.slice(0, sepIndex);
		headersText = new TextDecoder('utf-8').decode(headerBytes);
		body = u8.slice(sepIndex);
		// First status line like: HTTP/3 200 ... or HTTP/2 200
		const firstLine = headersText.split(/\r?\n/)[0] || '';
		const m = firstLine.match(/HTTP\/[0-9.]+\s+(\d{3})/);
		status = m ? parseInt(m[1], 10) : 0;
	}
	return { status, headersText, bodyBytes: body };
}

export default class CurlCommunication extends Communication {
	constructor() {
		super();
		this._cid = 'curl:subprocess';
	}

	connectionId() {
		return this._cid;
	}

	async connect() {
		// Best-effort check: if curl missing, spawn will fail at sendRequest time.
		this._connected = true;
		return true;
	}

	async close() {
		this._connected = false;
		return true;
	}

	async sendRequest(sid, request, data = null, _options = {}) {
		const scheme = request.scheme || 'https';
		const authority = request.authority || '127.0.0.1:12345';
		const path = request.path || '/';
		const method = (request.method || 'POST').toUpperCase();
		const url = `${scheme}://${authority}${path}`;
		const hasBody = !!(data && data.byteLength);
		const args = buildCurlArgs(url, method, hasBody);

		const { timeoutMs = 10000 } = _options || {};
		return new Promise((resolve) => {
			const child = spawn('curl', args, { stdio: ['pipe', 'pipe', 'pipe'] });
			const stdoutChunks = [];
			const stderrChunks = [];
			let timeoutId = null;

			child.stdout.on('data', (chunk) => stdoutChunks.push(chunk));
			child.stderr.on('data', (chunk) => stderrChunks.push(chunk));

			child.on('error', (err) => {
				const msg = `curl spawn error: ${String(err && err.message ? err.message : err)}`;
				this._emitResponseEvent(sid, { type: 'response', ok: false, status: 0, error: msg, data: new Uint8Array() });
				resolve({ ok: false, status: 0, error: msg });
			});

			child.on('close', (_code) => {
				if (timeoutId) { clearTimeout(timeoutId); timeoutId = null; }
				const stdout = Buffer.concat(stdoutChunks);
				const stderrText = Buffer.concat(stderrChunks).toString('utf-8');
				let status = 0;
				let body = new Uint8Array();
				try {
					const parsed = parseCurlHttpOutput(stdout);
					status = parsed.status || 0;
					body = new Uint8Array(parsed.bodyBytes);
				} catch {
					// Fallback: deliver raw stdout as body
					body = new Uint8Array(stdout);
				}
				const ok = status >= 200 && status < 300;
				this._emitResponseEvent(sid, { type: 'response', ok, status, data: body, stderr: stderrText });
				resolve({ ok, status, data: body });
			});

			// Write request body if present
			if (hasBody) {
				child.stdin.write(Buffer.from(data));
			}
			child.stdin.end();

			// Enforce timeout; kill curl and resolve with timeout error
			if (timeoutMs > 0) {
				timeoutId = setTimeout(() => {
					try { child.kill('SIGKILL'); } catch (_) {}
					const msg = `curl timeout after ${timeoutMs}ms`;
					this._emitResponseEvent(sid, { type: 'response', ok: false, status: 0, error: msg, data: new Uint8Array() });
					resolve({ ok: false, status: 0, error: msg, data: new Uint8Array() });
				}, timeoutMs);
			}
		});
	}
}

// Helper: send a heartbeat burst for a given request id and URL using curl.
// We encode a single payload_chunk with SIGNAL_HEARTBEAT and zero-length payload.
// Note: Heartbeats are triggered from the updater by sending a zero-length
// WWATP payload chunk with SIGNAL_HEARTBEAT and the same logical request id.
// This adapter simply sends whatever bytes are provided; see updater logic.
