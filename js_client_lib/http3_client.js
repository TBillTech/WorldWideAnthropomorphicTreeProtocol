// Http3ClientBackend  JS port of C++ client backend orchestrator
// Responsibilities:
// - Wrap a local Backend (cache) and send WWATP requests using HTTP3TreeMessage
// - Maintain a queue of pending requests for an Updater/transport to flush
// - Provide optional blockingMode: await response promises per requestId
// - Handle journaling bookkeeping (scaffold)

// Notes on the local Backend, and the purpose of the Journaling system
// * There is a local copy of the backend tree stored in this.localBackend_ .
// * Functions that do not mutate the tree assume and rely on up to date information being previously stored in the localBackend_ .
//    These functions are: getNode, getPageTree, relativeGetPageTree, queryNodes, and relativeQueryNodes.
// * Nodes in the tree that are considred mutable are the only nodes that the server is expected to change and cause the local to be invalidated.
// * Keeping the mutable nodes up to date is the job of the Journaling system
// * The updater periodically solicits JournalRequests, which send the server the current "state" of the client
// * In the happy path, the server responds with a JournalResponse carrying the latest updates.
// * If however, the client is too far behind, or the server resets, then the JournalResponse is truncated out-of-order
// * When the client receives a truncated out-of-order, it should try first to send a MutablePageTreeRequest to download exactly the mutated nodes.
// * Otherwise, if the MutablePageTree is not defined, then it will fall back on a full page tree request.
// * Requesting the MutablePageTree from the server is just a getPageTree request of the MutablePageTreeLabelRule_.

import { Backend } from './interface/backend.js';
import { Maybe, Just, Nothing } from './interface/maybe.js';
import HTTP3TreeMessage from './interface/http3_tree_message.js';
import { WWATP_SIGNAL, encodeToChunks, decodeFromChunks } from './interface/http3_tree_message_helpers.js';
import { decodeChunks_MaybeTreeNode } from './interface/http3_tree_message_helpers.js';
import Request from './transport/request.js';

let GLOBAL_REQUEST_ID = 1;

export default class Http3ClientBackend extends Backend {
	constructor(localBackend, blockingMode, request, journalRequestsPerMinute = 0, staticNode = Nothing) {
		super();
		if (!(localBackend instanceof Backend)) throw new TypeError('localBackend must implement Backend');
		this.localBackend_ = localBackend;
		this.blockingMode_ = !!blockingMode;
		this.requestUrlInfo_ = request instanceof Request ? request : new Request(request || {});
		this.journalRequestsPerMinute_ = Number(journalRequestsPerMinute) >>> 0;
		this.staticNode_ = Maybe.isMaybe(staticNode) ? staticNode : (staticNode ? Just(staticNode) : Nothing);

		this.pendingRequests_ = []; // queue of HTTP3TreeMessage
		this.waits_ = new Map(); // requestId -> { resolve, reject, type }
		this.staticRequestId_ = 0; // request id used for static asset fetch, if any

		// journaling state
		this.lastNotification_ = { signalCount: 0, notification: { labelRule: '', maybeNode: Nothing } };
		this.lastJournalRequestTime_ = 0;
		this.journalRequestWaiting_ = false;
		this.serverSyncOn_ = true;
		this.needMutablePageTreeSync_ = false;
		this.mutablePageTreeLabelRule_ = 'MutableNodes';
		this.notificationBlock_ = false;

		// validations
		if (this.staticNode_.isJust && this.staticNode_.isJust() && this.requestUrlInfo_.isWWATP()) {
			throw new Error('Static node must be used with static request (not WWATP).');
		}
		if (this.staticNode_.isJust && this.staticNode_.isJust() && this.journalRequestsPerMinute_ > 0) {
			throw new Error('Static node mode should not be used with journaling.');
		}
		if (this.staticNode_.isJust && this.staticNode_.isJust() && !this.blockingMode_) {
			this.requestStaticNodeData();
		}
	}

	// ---- Helpers: request lifecycle ----
	_nextRequestId() { return (GLOBAL_REQUEST_ID = (GLOBAL_REQUEST_ID + 1) & 0xffff) || (GLOBAL_REQUEST_ID = 1); }

	_enqueue(msg, waitType = null) {
		// Attach a fresh request id if not set
		if (!msg.requestId) msg.setRequestId(this._nextRequestId());
		this.pendingRequests_.push(msg);
		if (this.blockingMode_ && waitType) {
			const reqId = msg.requestId;
			return new Promise((resolve, reject) => {
				this.waits_.set(reqId, { resolve, reject, type: waitType });
			});
		}
		return Promise.resolve(true);
	}

	hasNextRequest() { return this.pendingRequests_.length > 0; }
	popNextRequest() { return this.pendingRequests_.shift(); }

	// For Updater/transport: build a journal request message
	solicitJournalRequest() {
		const msg = new HTTP3TreeMessage().setRequestId(this._nextRequestId());
		msg.encodeGetJournalRequest(this.lastNotification_);
		// mark as journal to handle special responses if needed
		msg.setJournalRequest(true);
		this.journalRequestWaiting_ = true;
		return msg;
	}

	getRequestUrlInfo() { return this.requestUrlInfo_; }

	// ---- Blocking wait helpers (public API parity methods call these via _enqueue) ----
	async awaitBoolResponse(promise) {
		const res = await promise; // bool or data depending on pipeline
		return !!res;
	}
	async awaitTreeResponse(promise) { return promise; }
	async awaitVectorTreeResponse(promise) { return promise; }

	// ---- Backend methods wiring ----
	getStaticNode() { return this.staticNode_; }

	getNode(labelRule) {
		// Non-mutating: rely on local backend state only; do not send or block.
		return this.localBackend_.getNode(String(labelRule));
	}

	upsertNode(nodes) {
		// Eagerly update local cache to reflect intended state; server/journal will converge if needed.
		try { this.localBackend_.upsertNode(nodes); } catch (_) {}
		const msg = new HTTP3TreeMessage().setRequestId(this._nextRequestId()).encodeUpsertNodeRequest(nodes);
		const waitP = this._enqueue(msg, 'bool');
		return this.blockingMode_ ? this.awaitBoolResponse(waitP) : true;
	}

	deleteNode(labelRule) {
		// Reflect deletion locally immediately; server/journal will confirm
		try { this.localBackend_.deleteNode(String(labelRule)); } catch (_) {}
		const msg = new HTTP3TreeMessage().setRequestId(this._nextRequestId()).encodeDeleteNodeRequest(String(labelRule));
		const waitP = this._enqueue(msg, 'bool');
		return this.blockingMode_ ? this.awaitBoolResponse(waitP) : true;
	}

	getPageTree(pageNodeLabelRule) {
		// Non-mutating: rely on local backend only; do not send or block.
		return this.localBackend_.getPageTree(String(pageNodeLabelRule));
	}

	relativeGetPageTree(node, pageNodeLabelRule) {
		// Non-mutating: ask local backend for relative page tree.
		return this.localBackend_.relativeGetPageTree(node, String(pageNodeLabelRule));
	}

	queryNodes(labelRule) {
		// Non-mutating: rely on local backend only; do not send or block.
		return this.localBackend_.queryNodes(String(labelRule));
	}

	relativeQueryNodes(node, labelRule) { return this.localBackend_.relativeQueryNodes(node, String(labelRule)); }

	openTransactionLayer(node) {
		const msg = new HTTP3TreeMessage().setRequestId(this._nextRequestId()).encodeOpenTransactionLayerRequest(node);
		const waitP = this._enqueue(msg, 'bool');
		return this.blockingMode_ ? this.awaitBoolResponse(waitP) : true;
	}

	closeTransactionLayers() {
		const msg = new HTTP3TreeMessage().setRequestId(this._nextRequestId()).encodeCloseTransactionLayersRequest();
		const waitP = this._enqueue(msg, 'bool');
		return this.blockingMode_ ? this.awaitBoolResponse(waitP) : true;
	}

	applyTransaction(transaction) {
		const msg = new HTTP3TreeMessage().setRequestId(this._nextRequestId()).encodeApplyTransactionRequest(transaction);
		const waitP = this._enqueue(msg, 'bool');
		// Block in blockingMode until server responds
		return this.blockingMode_ ? this.awaitBoolResponse(waitP) : true;
	}

	getFullTree() {
        return this.localBackend_.getFullTree();
	}

	registerNodeListener(listenerName, labelRule, childNotify, cb) {
		// For local listeners, register on local backend directly
		this.localBackend_.registerNodeListener(listenerName, labelRule, childNotify, cb);
		const msg = new HTTP3TreeMessage().setRequestId(this._nextRequestId()).encodeRegisterNodeListenerRequest(String(listenerName), String(labelRule), !!childNotify);
		const waitP = this._enqueue(msg, 'bool');
		return this.blockingMode_ ? this.awaitBoolResponse(waitP) : undefined;
	}

	deregisterNodeListener(listenerName, labelRule) {
		this.localBackend_.deregisterNodeListener(listenerName, labelRule);
		const msg = new HTTP3TreeMessage().setRequestId(this._nextRequestId()).encodeDeregisterNodeListenerRequest(String(listenerName), String(labelRule));
		const waitP = this._enqueue(msg, 'bool');
		return this.blockingMode_ ? this.awaitBoolResponse(waitP) : undefined;
	}

	notifyListeners(labelRule, maybeNode) {
		// Local side effect
		this.localBackend_.notifyListeners(labelRule, maybeNode);
		const msg = new HTTP3TreeMessage().setRequestId(this._nextRequestId()).encodeNotifyListenersRequest(String(labelRule), maybeNode);
		const waitP = this._enqueue(msg, 'bool');
		return this.blockingMode_ ? this.awaitBoolResponse(waitP) : undefined;
	}

	processNotifications() {
		this.localBackend_.processNotifications();
		const msg = new HTTP3TreeMessage().setRequestId(this._nextRequestId()).encodeProcessNotificationRequest();
		const waitP = this._enqueue(msg, 'bool');
		return this.blockingMode_ ? this.awaitBoolResponse(waitP) : undefined;
	}

	// ---- Response handling (called by Updater) ----
	processHTTP3Response(http3TreeMessage) {
		// Decide type by looking up waiter; then decode and resolve.
		const reqId = http3TreeMessage.requestId;
		const waiter = this.waits_.get(reqId);
		const signal = http3TreeMessage.signal;
		// Decode by signal family when no waiter type (e.g., journal in non-blocking)
		const finish = (value) => { if (waiter) { this.waits_.delete(reqId); waiter.resolve(value); } };
		const completeBool = () => {
			const bytes = decodeFromChunks(http3TreeMessage.responseChunks);
			if (!bytes || bytes.byteLength === 0) { finish(true); return; }
			try {
				const s = new TextDecoder('utf-8').decode(bytes).trim();
				if (s.length === 0) { finish(true); return; }
				if (/^(true|1)$/i.test(s)) { finish(true); return; }
				if (/^(false|0)$/i.test(s)) { finish(false); return; }
				// Fallback: treat non-zero first byte as truthy
				finish((bytes[0] | 0) !== 0);
			} catch {
				finish((bytes[0] | 0) !== 0);
			}
		};

		switch (signal) {
			case WWATP_SIGNAL.SIGNAL_WWATP_RESPONSE_CONTINUE: {
				// Accumulate payload; wait for FINAL before acting.
				return false;
			}
			case WWATP_SIGNAL.SIGNAL_WWATP_RESPONSE_FINAL: {
				// Final chunk for non-WWATP flows (e.g., static asset). Only act if this matches our static request.
				if (!this.staticRequestId_ || reqId !== this.staticRequestId_) {
					// Not a static asset response we initiated; nothing to do here.
					if (waiter) finish(true);
					return true;
				}
				try {
					// Attempt chunk-based Maybe<TreeNode> decode if the server returned such a payload
					const { value: nodeMaybe } = decodeChunks_MaybeTreeNode(0, http3TreeMessage.responseChunks);
					if (nodeMaybe && nodeMaybe.isJust && nodeMaybe.isJust()) {
						const node = nodeMaybe.getOrElse(null);
						this.staticNode_ = nodeMaybe;
						try { this.localBackend_.upsertNode([node]); } catch (_) {}
					}
				} catch (_) {
					// Ignore if not a chunk-based Maybe<TreeNode>
				} finally {
					this.staticRequestId_ = 0;
				}
				if (waiter) finish(true);
				return true;
			}
			case WWATP_SIGNAL.SIGNAL_WWATP_GET_NODE_RESPONSE: {
				const val = http3TreeMessage.decodeGetNodeResponse();
				// Update local cache on Just
				if (val.isJust && val.isJust()) {
					this.localBackend_.upsertNode([val.getOrElse(null)]);
				}
				finish(val);
				break;
			}
			case WWATP_SIGNAL.SIGNAL_WWATP_UPSERT_NODE_RESPONSE:
				finish(true);
				break;
			case WWATP_SIGNAL.SIGNAL_WWATP_DELETE_NODE_RESPONSE:
			case WWATP_SIGNAL.SIGNAL_WWATP_OPEN_TRANSACTION_LAYER_RESPONSE:
			case WWATP_SIGNAL.SIGNAL_WWATP_CLOSE_TRANSACTION_LAYERS_RESPONSE:
			case WWATP_SIGNAL.SIGNAL_WWATP_NOTIFY_LISTENERS_RESPONSE:
			case WWATP_SIGNAL.SIGNAL_WWATP_PROCESS_NOTIFICATION_RESPONSE: {
				completeBool();
				break;
			}
			case WWATP_SIGNAL.SIGNAL_WWATP_GET_PAGE_TREE_RESPONSE:
			case WWATP_SIGNAL.SIGNAL_WWATP_QUERY_NODES_RESPONSE:
			case WWATP_SIGNAL.SIGNAL_WWATP_GET_FULL_TREE_RESPONSE: {
				// vec<TreeNode>
				let vec;
				if (signal === WWATP_SIGNAL.SIGNAL_WWATP_GET_PAGE_TREE_RESPONSE) vec = http3TreeMessage.decodeGetPageTreeResponse();
				else if (signal === WWATP_SIGNAL.SIGNAL_WWATP_QUERY_NODES_RESPONSE) vec = http3TreeMessage.decodeGetQueryNodesResponse();
				else vec = http3TreeMessage.decodeGetFullTreeResponse();
				// Update cache on full tree or page fetch if desired; for now, upsert all
				if (Array.isArray(vec) && vec.length) this.localBackend_.upsertNode(vec);
				// If this page tree was requested as part of a journal catch-up, clear notification block
				if (signal === WWATP_SIGNAL.SIGNAL_WWATP_GET_PAGE_TREE_RESPONSE && http3TreeMessage.isJournalRequest) {
					this.notificationBlock_ = false;
				}
				finish(vec);
				break;
			}
			case WWATP_SIGNAL.SIGNAL_WWATP_APPLY_TRANSACTION_RESPONSE: {
				completeBool();
				break;
			}
			case WWATP_SIGNAL.SIGNAL_WWATP_GET_JOURNAL_RESPONSE: {
					const notifications = http3TreeMessage.decodeGetJournalResponse();
					if (!Array.isArray(notifications) || notifications.length === 0) {
						this.journalRequestWaiting_ = false;
						if (waiter) finish(true);
						break;
					}
					// Detect gaps relative to our last seen sequence, but always apply notifications.
					const SENTINEL = 0xFFFFFFFF >>> 0;
					let expected = (this.lastNotification_?.signalCount >>> 0) || 0;
					let notificationGap = false;
					let lastNonSentinel = null;
					for (const sn of notifications) {
						// Apply to local cache regardless of gap status
						this.processOneNotification(sn);
						const seq = (sn?.signalCount >>> 0);
						if (seq !== SENTINEL) {
							if (seq !== ((expected + 1) >>> 0)) notificationGap = true;
							expected = seq;
							lastNonSentinel = sn;
						}
					}
					if (notificationGap) {
						// Prefer a targeted mutable page tree sync when defined; otherwise full tree.
						const rule = String(this.mutablePageTreeLabelRule_ || '');
						if (rule.length > 0) this.requestPageTree(rule); else this.requestFullTreeSync();
					} else {
						// No gap -> lift any notification block immediately
						this.notificationBlock_ = false;
					}
					if (lastNonSentinel) this.updateLastNotification(lastNonSentinel);
					this.journalRequestWaiting_ = false;
					if (waiter) finish(true);
					break;
			}
			default: {
				// Unknown signal: treat as success for boolean-style waits to be resilient to
				// intermediary acks or transport-level frames in polyfill environments.
				if (waiter) finish(true);
				break;
			}
		}
		return true;
	}

	// ---- Journal + notifications ----
	processOneNotification(notification) {
		const { labelRule, maybeNode } = notification.notification;
		if (Maybe.isMaybe(maybeNode) && maybeNode.isNothing()) {
			this.localBackend_.deleteNode(labelRule);
		} else if (Maybe.isMaybe(maybeNode) && maybeNode.isJust()) {
			this.localBackend_.upsertNode([maybeNode.getOrElse(null)]);
		}
	}

	updateLastNotification(notification) { this.lastNotification_ = notification; }

	needToSendJournalRequest(time) {
		if (!this.serverSyncOn_) return false;
		if (this.journalRequestsPerMinute_ <= 0) return false;
		if (this.journalRequestWaiting_) return false;
		const interval = 60.0 / this.journalRequestsPerMinute_;
		if (this.lastJournalRequestTime_ === 0) return true; // fire first poll
		return (time - this.lastJournalRequestTime_) >= interval;
	}

	setJournalRequestComplete(time = null) {
		this.lastJournalRequestTime_ = (typeof time === 'number' && Number.isFinite(time)) ? time : (Date.now() / 1000.0);
		this.journalRequestWaiting_ = false;
	}

	requestFullTreeSync() {
		const msg = new HTTP3TreeMessage().setRequestId(this._nextRequestId()).encodeGetFullTreeRequest();
		return this._enqueue(msg, this.blockingMode_ ? 'vecTree' : null);
	}
	// ---- Mutable page tree helpers (minimal) ----
	setMutablePageTreeLabelRule(label = 'MutableNodes') { this.mutablePageTreeLabelRule_ = String(label || 'MutableNodes'); }
	getMutablePageTree() { return this.getPageTree(this.mutablePageTreeLabelRule_); }

	// ---- Local helper to request a page tree from server without blocking
	// Used by journaling when an out-of-order JournalResponse is received.
	requestPageTree(pageNodeLabelRule, options = {}) {
		// Enqueue a non-blocking server fetch to refresh local cache.
		// Returns true immediately; updater will handle response and update localBackend_.
		const rule = String(pageNodeLabelRule ?? this.mutablePageTreeLabelRule_);
		const msg = new HTTP3TreeMessage()
			.setRequestId(this._nextRequestId())
			.encodeGetPageTreeRequest(rule);
		if (options && options.journal) msg.setJournalRequest(true);
		// Do not create a waiter; this is fire-and-forget for cache warm-up
		this.pendingRequests_.push(msg);
        // Set the notification block to avoid sending notifications back to the server upon response
		this.notificationBlock_ = true;
		return true;
	}

	// ---- Static node fetch for non-blocking mode ----
	requestStaticNodeData() {
		// Only for non-WWATP paths; sends a minimal REQUEST_FINAL and expects RESPONSE_FINAL with a TreeNode payload.
		if (this.requestUrlInfo_.isWWATP()) return;
		const msg = new HTTP3TreeMessage().setRequestId(this._nextRequestId());
		// Empty payload; use REQUEST_FINAL to denote request completion
		msg.requestChunks = encodeToChunks(new Uint8Array(0), { signal: WWATP_SIGNAL.SIGNAL_WWATP_REQUEST_FINAL, requestId: msg.requestId });
		msg.isInitialized = true;
		msg.requestComplete = true;
		this.staticRequestId_ = msg.requestId;
		// Do not wait; updater will deliver response and processHTTP3Response will handle FINAL
		this.pendingRequests_.push(msg);
	}
}
