// Http3ClientBackendUpdater – orchestrates flushing backend requests over a Communication adapter
// Mirrors the C++ Frontend-based updater: holds many Http3ClientBackend instances, acquires
// stream identifiers per request, sends wire-encoded chunks, and routes responses back.

import Http3ClientBackend from './http3_client.js';
import { chunkFromWire, chunkToWire, WWATP_SIGNAL, decodeFromChunks, canDecodeChunks_MaybeTreeNode, canDecodeChunks_VectorTreeNode, can_decode_vec_sequential_notification, PayloadChunkHeader } from './interface/http3_tree_message_helpers.js';

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
		this._hbTimers = new Map(); // sidKey -> interval id for heartbeats
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
		// clear any active heartbeat timers
		for (const [, id] of this._hbTimers) clearInterval(id);
		this._hbTimers.clear();
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
		let finished = false;

		// Register response handler first to avoid races
			const handleEvent = (evt) => {
			let shouldCleanup = false;
			try {
				if (evt?.data instanceof Uint8Array) {
					// Parse wire bytes into chunks
					const bytes = evt.data;
					let o = 0;
					let lastTyped = null;
					let sawFinal = false;
					while (o < bytes.byteLength) {
						const { chunk, read } = chunkFromWire(bytes.slice(o));
						// Normalize request id to align with the original message id used for waiters
						try { if (chunk?.header && 'request_id' in chunk.header) chunk.header.request_id = msg.requestId; } catch (_) {}
						// Track typed vs terminal signals
						try {
							const sig = chunk.header?.signal;
							if (sig === WWATP_SIGNAL.SIGNAL_WWATP_RESPONSE_FINAL) sawFinal = true;
							else if (sig !== undefined && sig !== WWATP_SIGNAL.SIGNAL_WWATP_RESPONSE_CONTINUE) lastTyped = sig;
						} catch (_) {}
						msg.pushResponseChunk(chunk);
						o += read;
					}

					// Gate decoding using can_decode_* based on the original request's signal.
					const reqSig = (msg.requestChunks && msg.requestChunks[0] && msg.requestChunks[0].header && msg.requestChunks[0].header.signal) | 0;
		    const payload = decodeFromChunks(msg.responseChunks);
					let readySignal = null;
					switch (reqSig) {
						case WWATP_SIGNAL.SIGNAL_WWATP_GET_NODE_REQUEST:
			    if (canDecodeChunks_MaybeTreeNode(0, msg.responseChunks)) readySignal = WWATP_SIGNAL.SIGNAL_WWATP_GET_NODE_RESPONSE;
							break;
						case WWATP_SIGNAL.SIGNAL_WWATP_UPSERT_NODE_REQUEST:
							if (payload.byteLength >= 1) readySignal = WWATP_SIGNAL.SIGNAL_WWATP_UPSERT_NODE_RESPONSE;
							break;
						case WWATP_SIGNAL.SIGNAL_WWATP_DELETE_NODE_REQUEST:
							if (payload.byteLength >= 1) readySignal = WWATP_SIGNAL.SIGNAL_WWATP_DELETE_NODE_RESPONSE;
							break;
						case WWATP_SIGNAL.SIGNAL_WWATP_GET_PAGE_TREE_REQUEST:
							if (canDecodeChunks_VectorTreeNode(0, msg.responseChunks)) readySignal = WWATP_SIGNAL.SIGNAL_WWATP_GET_PAGE_TREE_RESPONSE;
							break;
						case WWATP_SIGNAL.SIGNAL_WWATP_QUERY_NODES_REQUEST:
							if (canDecodeChunks_VectorTreeNode(0, msg.responseChunks)) readySignal = WWATP_SIGNAL.SIGNAL_WWATP_QUERY_NODES_RESPONSE;
							break;
						case WWATP_SIGNAL.SIGNAL_WWATP_OPEN_TRANSACTION_LAYER_REQUEST:
							if (payload.byteLength >= 1) readySignal = WWATP_SIGNAL.SIGNAL_WWATP_OPEN_TRANSACTION_LAYER_RESPONSE;
							break;
						case WWATP_SIGNAL.SIGNAL_WWATP_CLOSE_TRANSACTION_LAYERS_REQUEST:
							if (payload.byteLength >= 1) readySignal = WWATP_SIGNAL.SIGNAL_WWATP_CLOSE_TRANSACTION_LAYERS_RESPONSE;
							break;
						case WWATP_SIGNAL.SIGNAL_WWATP_APPLY_TRANSACTION_REQUEST:
							if (payload.byteLength >= 1) readySignal = WWATP_SIGNAL.SIGNAL_WWATP_APPLY_TRANSACTION_RESPONSE;
							break;
						case WWATP_SIGNAL.SIGNAL_WWATP_GET_FULL_TREE_REQUEST:
							if (canDecodeChunks_VectorTreeNode(0, msg.responseChunks)) readySignal = WWATP_SIGNAL.SIGNAL_WWATP_GET_FULL_TREE_RESPONSE;
							break;
						case WWATP_SIGNAL.SIGNAL_WWATP_REGISTER_LISTENER_REQUEST:
							if (payload.byteLength >= 1) readySignal = WWATP_SIGNAL.SIGNAL_WWATP_REGISTER_LISTENER_RESPONSE;
							break;
						case WWATP_SIGNAL.SIGNAL_WWATP_DEREGISTER_LISTENER_REQUEST:
							if (payload.byteLength >= 1) readySignal = WWATP_SIGNAL.SIGNAL_WWATP_DEREGISTER_LISTENER_RESPONSE;
							break;
						case WWATP_SIGNAL.SIGNAL_WWATP_NOTIFY_LISTENERS_REQUEST:
							if (payload.byteLength >= 1) readySignal = WWATP_SIGNAL.SIGNAL_WWATP_NOTIFY_LISTENERS_RESPONSE;
							break;
						case WWATP_SIGNAL.SIGNAL_WWATP_PROCESS_NOTIFICATION_REQUEST:
							// Empty allowed (treat as true) or bool
							if (payload.byteLength === 0 || payload.byteLength >= 1) readySignal = WWATP_SIGNAL.SIGNAL_WWATP_PROCESS_NOTIFICATION_RESPONSE;
							break;
						case WWATP_SIGNAL.SIGNAL_WWATP_GET_JOURNAL_REQUEST:
							if (can_decode_vec_sequential_notification(payload, 0)) readySignal = WWATP_SIGNAL.SIGNAL_WWATP_GET_JOURNAL_RESPONSE;
							break;
						default:
							break;
					}

					if (readySignal !== null) {
						msg.signal = readySignal;
						backend.processHTTP3Response(msg);
						shouldCleanup = true;
					} else {
						// Fallback: Prefer a typed response signal if present; otherwise mark FINAL for static asset flows.
						if (lastTyped !== null) msg.signal = lastTyped; else if (sawFinal) msg.signal = WWATP_SIGNAL.SIGNAL_WWATP_RESPONSE_FINAL;
						// If we got a typed non-continue signal or a FINAL, we can also clean up; otherwise keep waiting for more chunks.
						if (msg.signal === WWATP_SIGNAL.SIGNAL_WWATP_RESPONSE_FINAL || (lastTyped !== null && lastTyped !== WWATP_SIGNAL.SIGNAL_WWATP_RESPONSE_CONTINUE)) {
							backend.processHTTP3Response(msg);
							shouldCleanup = true;
						} else {
							// Not enough yet; keep handler registered for more data
							return;
						}
					}
				}
				// No-op here; handled above conditionally
			} finally {
				// Cleanup only when response completed
				try {
					if (typeof shouldCleanup !== 'undefined' && shouldCleanup) {
						finished = true;
						connector.deregisterResponseHandler(sid);
						this.ongoingRequests_.delete(sidKey);
						if (isJournal) backend.setJournalRequestComplete(this.lastTime_);
						// stop any heartbeat for this request
						const t = this._hbTimers.get(sidKey); if (t) clearInterval(t); this._hbTimers.delete(sidKey);
					}
				} catch (_) {}
			}
		};
		connector.registerResponseHandler(sid, handleEvent);

		// Build request wire bytes by concatenating all request chunks
		const parts = [];
		for (const chunk of msg.requestChunks) parts.push(chunkToWire(chunk));
		// Important for curl-based transport: append a zero-length HEARTBEAT chunk
		// immediately after the request on WWATP endpoints. This ensures servers that
		// flush responses only after seeing a follow-up message on the same stream
		// will produce a response within this single request burst.
		try {
			const isWWATP = typeof request.isWWATP === 'function' ? request.isWWATP() : String(request.path || '').includes('wwatp/');
			if (isWWATP) {
				const hbHeader = new PayloadChunkHeader(msg.requestId & 0xffff, WWATP_SIGNAL.SIGNAL_HEARTBEAT & 0xff, 0);
				parts.push(chunkToWire({ header: hbHeader, payload: new Uint8Array(0) }));
			}
		} catch (_) {}
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
			// Start heartbeat loop for WWATP requests after initial send; server may only release
			// results upon receiving a HEARTBEAT on a subsequent stream with the same logical request id.
			if (typeof request.isWWATP === 'function' ? request.isWWATP() : String(request.path || '').includes('wwatp/')) {
				const loop = async () => {
					if (finished) {
						const t = this._hbTimers.get(sidKey); if (t) clearInterval(t); this._hbTimers.delete(sidKey);
						return;
					}
					// If original handler no longer present, stop
					if (!connector.hasResponseHandler?.(sid)) {
						const t = this._hbTimers.get(sidKey); if (t) clearInterval(t); this._hbTimers.delete(sidKey);
						return;
					}
					try {
						const hbHeader = new PayloadChunkHeader(msg.requestId & 0xffff, WWATP_SIGNAL.SIGNAL_HEARTBEAT & 0xff, 0);
						const hbWire = chunkToWire({ header: hbHeader, payload: new Uint8Array(0) });
						const hbSid = connector.getNewRequestStreamIdentifier(request);
						const r = await connector.sendRequest(hbSid, request, hbWire, { timeoutMs: 2000 });
						if (r && r.data instanceof Uint8Array && r.data.byteLength) {
							// Feed returned bytes through the same handler logic
							handleEvent({ data: r.data });
						}
					} catch (_) { /* ignore heartbeat errors */ }
				};
				const id = setInterval(loop, 100);
				this._hbTimers.set(sidKey, id);
			}
		} catch {
			// On send error, clean up and reject waiter if any
			try { connector.deregisterResponseHandler(sid); } catch (_) {}
			this.ongoingRequests_.delete(sidKey);
			// Attempt to resolve a waiting promise with failure via processHTTP3Response default path
			try { backend.processHTTP3Response(msg); } catch (_) {}
		}
	}
}

