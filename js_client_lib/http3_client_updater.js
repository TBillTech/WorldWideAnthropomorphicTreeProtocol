// Http3ClientBackendUpdater – orchestrates flushing backend requests over a Communication adapter
// Mirrors the C++ Frontend-based updater: holds many Http3ClientBackend instances, acquires
// stream identifiers per request, sends wire-encoded chunks, and routes responses back.

/* eslint-env node */
import Http3ClientBackend from './http3_client.js';
import { tracer } from './transport/instrumentation.js';
import { chunkFromWire, chunkToWire, WWATP_SIGNAL, SIGNAL_HEARTBEAT, decodeFromChunks, canDecodeChunks_MaybeTreeNode, canDecodeChunks_VectorTreeNode, canDecodeChunks_VectorSequentialNotification, PayloadChunkHeader } from './interface/http3_tree_message_helpers.js';

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
		this._trace = tracer('Updater');
		this._lastMaintainLogTs = 0; // wall-clock ms for throttling maintain.start logs
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
	async maintainRequestHandlers(connector, time = 0) {
		this.lastTime_ = Number(time) || 0;
		// Throttle noisy trace to at most once per second
		try {
			const now = Date.now();
			if (!this._lastMaintainLogTs || (now - this._lastMaintainLogTs) >= 1000) {
				this._lastMaintainLogTs = now;
				this._trace.info('maintain.start', { time: this.lastTime_ });
			}
		} catch (_) {
			// best-effort logging only
		}

		// 1) Flush normal pending requests
		const pending = [];
		for (const backend of this.backends_) {
			while (backend.hasNextRequest()) {
				const msg = backend.popNextRequest();
				this._trace.info('dispatch.request', { rid: msg.requestId, backend: backend.getName?.() });
				pending.push(this.#dispatchOne(connector, backend, msg, /*isJournal*/ false));
			}
		}

		// 2) Periodically solicit journal requests
		for (const backend of this.backends_) {
			if (backend.needToSendJournalRequest(this.lastTime_)) {
				const jmsg = backend.solicitJournalRequest();
				this._trace.info('dispatch.journal', { rid: jmsg.requestId, backend: backend.getName?.() });
				pending.push(this.#dispatchOne(connector, backend, jmsg, /*isJournal*/ true));
			}
		}

		// 3) Like the C++ service's responseCycle, pump the connector's request/response queues once.
		try {
			if (typeof connector.processRequestStream === 'function') {
				connector.processRequestStream();
			}
		} catch (_) { /* best-effort only */ }

		// Await all dispatched requests to complete (response received and processed)
		if (pending.length) {
			try { await Promise.allSettled(pending); } catch (_) { /* ignore */ }
		}
	}

	// Internal: send a single HTTP3TreeMessage over the connector
	async #dispatchOne(connector, backend, msg, isJournal) {
		const request = backend.getRequestUrlInfo();
		const sid = connector.getNewRequestStreamIdentifier(request);
		const logicalReqId = (sid && sid.logicalId) ? (sid.logicalId & 0xffff) : (msg.requestId & 0xffff);
		const sidKey = String(sid);
		let finished = false;
		let doneResolve;
		const donePromise = new Promise((res) => { doneResolve = res; });
		this._trace.info('send.start', { sid, rid: msg.requestId, isJournal });

		// Register response handler first to avoid races
			const handleEvent = (evt) => {
			let shouldCleanup = false;
			try {
				if (evt?.type === 'end') {
					// Transport signaled remote FIN with no further data.
					// For journals, this corresponds to an empty FINAL-only response; mark complete.
					// For boolish WWATP requests, also treat as complete.
					const reqSig = (msg.requestChunks && msg.requestChunks[0] && msg.requestChunks[0].header && msg.requestChunks[0].header.signal) | 0;
					const boolish = (
						reqSig === WWATP_SIGNAL.SIGNAL_WWATP_UPSERT_NODE_REQUEST ||
						reqSig === WWATP_SIGNAL.SIGNAL_WWATP_DELETE_NODE_REQUEST ||
						reqSig === WWATP_SIGNAL.SIGNAL_WWATP_OPEN_TRANSACTION_LAYER_REQUEST ||
						reqSig === WWATP_SIGNAL.SIGNAL_WWATP_CLOSE_TRANSACTION_LAYERS_REQUEST ||
						reqSig === WWATP_SIGNAL.SIGNAL_WWATP_NOTIFY_LISTENERS_REQUEST ||
						reqSig === WWATP_SIGNAL.SIGNAL_WWATP_PROCESS_NOTIFICATION_REQUEST ||
						reqSig === WWATP_SIGNAL.SIGNAL_WWATP_APPLY_TRANSACTION_REQUEST ||
						reqSig === WWATP_SIGNAL.SIGNAL_WWATP_GET_JOURNAL_REQUEST
					);
					if (boolish || isJournal) {
						msg.signal = (reqSig === WWATP_SIGNAL.SIGNAL_WWATP_GET_JOURNAL_REQUEST)
							? WWATP_SIGNAL.SIGNAL_WWATP_GET_JOURNAL_RESPONSE
							: WWATP_SIGNAL.SIGNAL_WWATP_RESPONSE_FINAL;
						backend.processHTTP3Response(msg);
						shouldCleanup = true;
					}
					return;
				}
				if (evt?.data instanceof Uint8Array) {
					// Parse wire bytes into chunks
					const bytes = evt.data;
					let o = 0;
					let lastTyped = null;
					let sawFinal = false;
					// Determine the original request signal once for filtering
					const reqSig = (msg.requestChunks && msg.requestChunks[0] && msg.requestChunks[0].header && msg.requestChunks[0].header.signal) | 0;
					const isWWATPReq = (reqSig >= WWATP_SIGNAL.SIGNAL_WWATP_GET_NODE_REQUEST && reqSig <= WWATP_SIGNAL.SIGNAL_WWATP_GET_JOURNAL_REQUEST);
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
						// Filter out transport heartbeats from accumulation to avoid corrupting payload decoding
						try {
							const sig = chunk.header?.signal;
							if (sig === SIGNAL_HEARTBEAT) {
								// Ignore heartbeat frames entirely
								o += read; continue;
							}
							// Do not exclude WWATP RESPONSE_FINAL; some servers reply with FINAL-only for boolean ops
						} catch (_) {}
						msg.pushResponseChunk(chunk);
						o += read;
					}

					// Gate decoding using can_decode_* based on the original request's signal.
					let readySignal = null;
					switch (reqSig) {
						case WWATP_SIGNAL.SIGNAL_WWATP_GET_NODE_REQUEST:
			                if (canDecodeChunks_MaybeTreeNode(0, msg.responseChunks)) readySignal = WWATP_SIGNAL.SIGNAL_WWATP_GET_NODE_RESPONSE;
						    break;
						case WWATP_SIGNAL.SIGNAL_WWATP_UPSERT_NODE_REQUEST: {
							// Consider ready when we see a typed UPSERT_NODE_RESPONSE chunk or any non-empty payload
							const hasTyped = !!msg.responseChunks.find((c) => (c?.header?.signal | 0) === WWATP_SIGNAL.SIGNAL_WWATP_UPSERT_NODE_RESPONSE);
							if (hasTyped) readySignal = WWATP_SIGNAL.SIGNAL_WWATP_UPSERT_NODE_RESPONSE;
							    break;
						    }
							break;
						case WWATP_SIGNAL.SIGNAL_WWATP_DELETE_NODE_REQUEST:
							if (msg.responseChunks.length >= 1) readySignal = WWATP_SIGNAL.SIGNAL_WWATP_DELETE_NODE_RESPONSE;
							break;
						case WWATP_SIGNAL.SIGNAL_WWATP_GET_PAGE_TREE_REQUEST:
							if (canDecodeChunks_VectorTreeNode(0, msg.responseChunks)) readySignal = WWATP_SIGNAL.SIGNAL_WWATP_GET_PAGE_TREE_RESPONSE;
							break;
						case WWATP_SIGNAL.SIGNAL_WWATP_QUERY_NODES_REQUEST:
							if (canDecodeChunks_VectorTreeNode(0, msg.responseChunks)) readySignal = WWATP_SIGNAL.SIGNAL_WWATP_QUERY_NODES_RESPONSE;
							break;
						case WWATP_SIGNAL.SIGNAL_WWATP_OPEN_TRANSACTION_LAYER_REQUEST:
							if (msg.responseChunks.length >= 1) readySignal = WWATP_SIGNAL.SIGNAL_WWATP_OPEN_TRANSACTION_LAYER_RESPONSE;
							break;
						case WWATP_SIGNAL.SIGNAL_WWATP_CLOSE_TRANSACTION_LAYERS_REQUEST:
							if (msg.responseChunks.length >= 1) readySignal = WWATP_SIGNAL.SIGNAL_WWATP_CLOSE_TRANSACTION_LAYERS_RESPONSE;
							break;
						case WWATP_SIGNAL.SIGNAL_WWATP_APPLY_TRANSACTION_REQUEST:
							if (msg.responseChunks.length >= 1) readySignal = WWATP_SIGNAL.SIGNAL_WWATP_APPLY_TRANSACTION_RESPONSE;
							break;
						case WWATP_SIGNAL.SIGNAL_WWATP_GET_FULL_TREE_REQUEST:
							if (canDecodeChunks_VectorTreeNode(0, msg.responseChunks)) readySignal = WWATP_SIGNAL.SIGNAL_WWATP_GET_FULL_TREE_RESPONSE;
							break;
						case WWATP_SIGNAL.SIGNAL_WWATP_REGISTER_LISTENER_REQUEST:
							if (msg.responseChunks.length >= 1) readySignal = WWATP_SIGNAL.SIGNAL_WWATP_REGISTER_LISTENER_RESPONSE;
							break;
						case WWATP_SIGNAL.SIGNAL_WWATP_DEREGISTER_LISTENER_REQUEST:
							if (msg.responseChunks.length >= 1) readySignal = WWATP_SIGNAL.SIGNAL_WWATP_DEREGISTER_LISTENER_RESPONSE;
							break;
						case WWATP_SIGNAL.SIGNAL_WWATP_NOTIFY_LISTENERS_REQUEST:
							if (msg.responseChunks.length >= 1) readySignal = WWATP_SIGNAL.SIGNAL_WWATP_NOTIFY_LISTENERS_RESPONSE;
							break;
						case WWATP_SIGNAL.SIGNAL_WWATP_PROCESS_NOTIFICATION_REQUEST:
							// Empty allowed (treat as true) or bool
							readySignal = WWATP_SIGNAL.SIGNAL_WWATP_PROCESS_NOTIFICATION_RESPONSE;
							break;
						case WWATP_SIGNAL.SIGNAL_WWATP_GET_JOURNAL_REQUEST: {
							if (canDecodeChunks_VectorSequentialNotification(0, msg.responseChunks)) {
								readySignal = WWATP_SIGNAL.SIGNAL_WWATP_GET_JOURNAL_RESPONSE;
							} else if (sawFinal) {
								// Some servers may reply with FINAL-only when the journal is empty.
								// Treat this as a valid (empty) journal response to avoid stalling.
								readySignal = WWATP_SIGNAL.SIGNAL_WWATP_GET_JOURNAL_RESPONSE;
							}
							break;
						}
						default:
							break;
					}

					if (readySignal !== null) {
						msg.signal = readySignal;
						backend.processHTTP3Response(msg);
						shouldCleanup = true;
					} else {
						// For WWATP requests, do not process typed response frames until canDecode marks the payload ready.
						// Early decode attempts can race with chunk accumulation and cause transient decode errors.
						if (isWWATPReq) {
							// Special-case: some WWATP ops complete with FINAL-only and no typed response.
							// If we saw a FINAL and the request was a boolean-style op, process now.
							if (sawFinal) {
								const boolish = (
									reqSig === WWATP_SIGNAL.SIGNAL_WWATP_UPSERT_NODE_REQUEST ||
									reqSig === WWATP_SIGNAL.SIGNAL_WWATP_DELETE_NODE_REQUEST ||
									reqSig === WWATP_SIGNAL.SIGNAL_WWATP_OPEN_TRANSACTION_LAYER_REQUEST ||
									reqSig === WWATP_SIGNAL.SIGNAL_WWATP_CLOSE_TRANSACTION_LAYERS_REQUEST ||
									reqSig === WWATP_SIGNAL.SIGNAL_WWATP_NOTIFY_LISTENERS_REQUEST ||
									reqSig === WWATP_SIGNAL.SIGNAL_WWATP_PROCESS_NOTIFICATION_REQUEST ||
									reqSig === WWATP_SIGNAL.SIGNAL_WWATP_APPLY_TRANSACTION_REQUEST
								);
								if (boolish) {
									msg.signal = WWATP_SIGNAL.SIGNAL_WWATP_RESPONSE_FINAL;
									backend.processHTTP3Response(msg);
									shouldCleanup = true;
									return;
								}
							}
							return; // wait for more chunks
						}
						// Non-WWATP flows: prefer a typed response signal if present; otherwise mark FINAL.
						if (lastTyped !== null) msg.signal = lastTyped; else if (sawFinal) msg.signal = WWATP_SIGNAL.SIGNAL_WWATP_RESPONSE_FINAL;
						if (msg.signal === WWATP_SIGNAL.SIGNAL_WWATP_RESPONSE_FINAL || (lastTyped !== null && lastTyped !== WWATP_SIGNAL.SIGNAL_WWATP_RESPONSE_CONTINUE)) {
							backend.processHTTP3Response(msg);
							shouldCleanup = true;
						} else {
							return; // keep waiting
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
						// Close the persistent stream for this sid if the transport supports it
						try {
							if (typeof connector.closeStream === 'function') {
								(async () => { try { await connector.closeStream(sid); } catch {} })();
							}
						} catch (_) {}
							try { if (doneResolve) doneResolve(true); } catch (_) {}
					}
				} catch (_) {}
			}
		};
		connector.registerResponseHandler(sid, handleEvent);

		// Send each request chunk separately to preserve chunk boundaries on the server side.
		// IMPORTANT: Align WWATP payload request_id with the stream logical id (server expects this)
		const perChunkWire = [];
		for (const chunk of msg.requestChunks) {
			try {
				const hdr = { ...(chunk.header || {}) };
				// Force request_id to logical stream id for on-wire semantics
				hdr.request_id = logicalReqId;
				perChunkWire.push({ wire: chunkToWire({ header: hdr, payload: chunk.payload }), signal: hdr.signal | 0 });
			} catch {
				// Fallback if header/payload shape unexpected
				const w = chunkToWire(chunk);
				const sig = (chunk && chunk.header && chunk.header.signal) | 0;
				perChunkWire.push({ wire: w, signal: sig });
			}
		}

		// Targeted debug: log the first request chunk header sizes
		try {
			const first = msg.requestChunks && msg.requestChunks[0];
			if (first && first.header && Object.prototype.hasOwnProperty.call(first.header, 'data_length')) {
				this._trace.info('send.arg.first', { sid, rid: msg.requestId, sig: first.header.signal, wire: (first.header.data_length | 0) + 6, payload_len: first.header.data_length | 0 });
			}
		} catch (_) {}

		// Track ongoing
		this.ongoingRequests_.set(sidKey, { sid, backend, msg, isJournal });

		// Fire request: send chunks sequentially to preserve boundaries
		try {
			for (let i = 0; i < perChunkWire.length; i++) {
				const { wire: w, signal } = perChunkWire[i];
				// Pass per-chunk signal to allow native layer to frame appropriately
				await connector.sendRequest(sid, request, w, { requestSignal: signal });
			}
			this._trace.info('send.done', { sid, rid: msg.requestId });
			// Start heartbeat loop for WWATP requests after initial send; server may only release
			// results upon receiving a HEARTBEAT on a subsequent stream with the same logical request id.
			const hbDisabled = (() => { try { const v = globalThis?.process?.env?.WWATP_DISABLE_HB; if (!v) return false; const s = String(v).toLowerCase(); return s === '1' || s === 'true' || s === 'yes' || s === 'on'; } catch { return false; } })();
			if (!hbDisabled && connector.hasResponseHandler?.(sid) && (typeof request.isWWATP === 'function' ? request.isWWATP() : String(request.path || '').includes('wwatp/'))) {
				let hbBusy = false;
				const loop = async () => {
					if (hbBusy) return; // avoid overlapping heartbeats on slow/native layers
					hbBusy = true;
					if (finished) {
						const t = this._hbTimers.get(sidKey); if (t) clearInterval(t); this._hbTimers.delete(sidKey);
						this._trace.info('hb.stop', { sid, rid: msg.requestId });
						hbBusy = false;
						return;
					}
					// If original handler no longer present, stop
					if (!connector.hasResponseHandler?.(sid)) {
						const t = this._hbTimers.get(sidKey); if (t) clearInterval(t); this._hbTimers.delete(sidKey);
						this._trace.info('hb.stop.missingHandler', { sid, rid: msg.requestId });
						hbBusy = false;
						return;
					}
					try {
						const hbHeader = new PayloadChunkHeader(logicalReqId, SIGNAL_HEARTBEAT & 0xff, 0);
						const hbWire = chunkToWire({ header: hbHeader, payload: new Uint8Array(0) });
						// Reuse the original StreamIdentifier for routing; open a fresh native stream for the heartbeat
						// while keeping the original response stream readable. Tag as heartbeat so the transport
						// can optimize (ephemeral write-only on Node emulator).
						const r = await connector.sendRequest(sid, request, hbWire, { timeoutMs: 2000, requestSignal: SIGNAL_HEARTBEAT, isHeartbeat: true });
						this._trace.inc('hb.sent');
						// Suppress verbose heartbeat tick logs to reduce noise
						// this._trace.info('hb.tick', { sid, rid: msg.requestId, bytes: r?.data?.byteLength || 0 });
						// Response bytes (if any) will be delivered to the original handler via _emitResponseEvent(sid,...)
					} catch (e) {
						// On write/transport error, stop heartbeats to avoid hammering a closed stream
						this._trace.warn('hb.error', { error: String(e && e.message || e) });
						try { const t = this._hbTimers.get(sidKey); if (t) clearInterval(t); this._hbTimers.delete(sidKey); } catch {}
						finished = true;
					}
					finally { hbBusy = false; }
				};
				const id = setInterval(loop, 100);
				this._hbTimers.set(sidKey, id);
			} else if (hbDisabled) {
				this._trace.info('hb.disabled', { sid, rid: msg.requestId });
			}
		} catch (e) {
			// On send error, clean up and reject waiter if any
			try { connector.deregisterResponseHandler(sid); } catch (_) {}
			this.ongoingRequests_.delete(sidKey);
			this._trace.warn('send.error', { sid, rid: msg.requestId, error: String(e && e.message || e) });
			// Attempt to resolve a waiting promise with failure via processHTTP3Response default path
			try { backend.processHTTP3Response(msg); } catch (_) {}
			try { if (doneResolve) doneResolve(false); } catch (_) {}
		}

		// Ensure caller can await completion even if no response arrives (timeouts not modeled here)
		return donePromise;
	}
}

