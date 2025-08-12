// Http3ClientBackendUpdater – orchestrates flushing backend requests over a Communication adapter
// Mirrors the C++ Frontend-based updater: holds many Http3ClientBackend instances, acquires
// stream identifiers per request, sends wire-encoded chunks, and routes responses back.

import Http3ClientBackend from './http3_client.js';
import { chunkFromWire, chunkToWire } from './interface/http3_tree_message_helpers.js';

export default class Http3ClientBackendUpdater {
	constructor(name = 'default', ipAddr = 'localhost', port = 443) {
		this.name_ = String(name);
		this.ipAddr_ = String(ipAddr);
		this.port_ = port | 0;

		this.backends_ = []; // Array<Http3ClientBackend>
		// key: sid.toString() -> { sid, backend, msg, isJournal }
		this.ongoingRequests_ = new Map();
		this.lastTime_ = 0;

		this._timer = null;
	}

	getName() { return `Http3ClientBackendUpdater_${this.name_}_${this.ipAddr_}_${this.port_}`; }
	getType() { return 'http3_client_backend_updater'; }

	// Add a backend to manage. Creates a JS Http3ClientBackend instance.
	addBackend(localBackend, blockingMode, request, journalRequestsPerMinute = 0, staticNode = null) {
		const be = new Http3ClientBackend(localBackend, !!blockingMode, request, journalRequestsPerMinute, staticNode);
		this.backends_.push(be);
		return be;
	}

	getBackend(url) {
		const u = String(url);
		const found = this.backends_.find((b) => {
			const r = b.getRequestUrlInfo();
			return r.path === u || String(r) === u || r.toString?.() === u;
		});
		if (!found) throw new Error(`Backend not found for url: ${u}`);
		return found;
	}

	getBackends() { return [...this.backends_]; }

	size() { return this.backends_.length; }

	isRunning() { return !!this._timer; }

	start(connector, time = 0, sleepMilli = 100) {
		if (this._timer) return; // already running
		let localTime = Number(time) || 0;
		this._timer = setInterval(() => {
			try {
				this.maintainRequestHandlers(connector, localTime);
				if (typeof connector.processRequestStream === 'function') {
					try { connector.processRequestStream(); } catch (_) { /* ignore */ }
				}
				localTime += 0.1;
			} catch (_) {
				// keep loop resilient
			}
		}, Math.max(1, sleepMilli | 0));
	}

	stop() {
		if (this._timer) {
			clearInterval(this._timer);
			this._timer = null;
		}
	}

	// Core: allocate streams for pending requests and periodic journal polls.
	maintainRequestHandlers(connector, time = 0) {
		this.lastTime_ = Number(time) || 0;

		// 1) Flush normal pending requests
		for (const backend of this.backends_) {
			while (backend.hasNextRequest()) {
				const msg = backend.popNextRequest();
				this.#dispatchOne(connector, backend, msg, /*isJournal*/ false);
			}
		}

		// 2) Periodically solicit journal requests
		for (const backend of this.backends_) {
			if (backend.needToSendJournalRequest(this.lastTime_)) {
				const jmsg = backend.solicitJournalRequest();
				this.#dispatchOne(connector, backend, jmsg, /*isJournal*/ true);
			}
		}
	}

	// Internal: send a single HTTP3TreeMessage over the connector
	async #dispatchOne(connector, backend, msg, isJournal) {
		const request = backend.getRequestUrlInfo();
		const sid = connector.getNewRequestStreamIdentifier(request);
		const sidKey = String(sid);

		// Register response handler first to avoid races
			connector.registerResponseHandler(sid, (evt) => {
			try {
				if (evt?.data instanceof Uint8Array) {
					// Parse wire bytes into chunks
					const bytes = evt.data;
					let o = 0;
						let first = true;
					while (o < bytes.byteLength) {
						const { chunk, read } = chunkFromWire(bytes.slice(o));
							// Normalize request id to align with the original message id used for waiters
							try { if (chunk?.header && 'request_id' in chunk.header) chunk.header.request_id = msg.requestId; } catch (_) {}
							if (first) { try { msg.signal = chunk.header?.signal ?? msg.signal; } catch (_) {} first = false; }
							msg.pushResponseChunk(chunk);
						o += read;
					}
				}
				// Let the backend decode and resolve waiters
				backend.processHTTP3Response(msg);
			} finally {
				connector.deregisterResponseHandler(sid);
				this.ongoingRequests_.delete(sidKey);
				if (isJournal) backend.setJournalRequestComplete();
			}
		});

		// Build request wire bytes by concatenating all request chunks
		const parts = [];
		for (const chunk of msg.requestChunks) parts.push(chunkToWire(chunk));
		const total = parts.reduce((s, p) => s + p.byteLength, 0);
		const wire = new Uint8Array(total);
		{
			let o = 0;
			for (const p of parts) { wire.set(p, o); o += p.byteLength; }
		}

		// Track ongoing
		this.ongoingRequests_.set(sidKey, { sid, backend, msg, isJournal });

		// Fire request
		try {
			await connector.sendRequest(sid, request, wire);
		} catch (e) {
			// On send error, clean up and reject waiter if any
			try { connector.deregisterResponseHandler(sid); } catch (_) {}
			this.ongoingRequests_.delete(sidKey);
			// Attempt to resolve a waiting promise with failure via processHTTP3Response default path
			try { backend.processHTTP3Response(msg); } catch (_) {}
		}
	}
}

