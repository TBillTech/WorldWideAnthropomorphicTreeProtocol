// Http3ClientBackend  JS port of C++ client backend orchestrator
// Responsibilities:
// - Wrap a local Backend (cache) and send WWATP requests using HTTP3TreeMessage
// - Maintain a queue of pending requests for an Updater/transport to flush
// - Provide optional blockingMode: await response promises per requestId
// - Handle journaling bookkeeping (scaffold)

import { Backend } from './interface/backend.js';
import { Maybe, Just, Nothing } from './interface/maybe.js';
import HTTP3TreeMessage from './interface/http3_tree_message.js';
import { WWATP_SIGNAL, encodeToChunks, decodeFromChunks, decode_maybe_tree_node, decode_tree_node } from './interface/http3_tree_message_helpers.js';
import { TreeNode } from './interface/tree_node.js';
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
		// For now, always send to server; local cache sync depends on journaling/full tree requests.
		const msg = new HTTP3TreeMessage().setRequestId(this._nextRequestId()).encodeGetNodeRequest(String(labelRule));
		const waitP = this._enqueue(msg, 'maybeTree');
		return this.blockingMode_ ? waitP : Nothing; // non-blocking returns Nothing as placeholder
	}

	upsertNode(nodes) {
		const msg = new HTTP3TreeMessage().setRequestId(this._nextRequestId()).encodeUpsertNodeRequest(nodes);
		const waitP = this._enqueue(msg, 'bool');
		return this.blockingMode_ ? this.awaitBoolResponse(waitP) : true;
	}

	deleteNode(labelRule) {
		const msg = new HTTP3TreeMessage().setRequestId(this._nextRequestId()).encodeDeleteNodeRequest(String(labelRule));
		const waitP = this._enqueue(msg, 'bool');
		return this.blockingMode_ ? this.awaitBoolResponse(waitP) : true;
	}

	getPageTree(pageNodeLabelRule) {
		const msg = new HTTP3TreeMessage().setRequestId(this._nextRequestId()).encodeGetPageTreeRequest(String(pageNodeLabelRule));
		const waitP = this._enqueue(msg, 'vecTree');
		return this.blockingMode_ ? this.awaitVectorTreeResponse(waitP) : [];
	}

	relativeGetPageTree(node, pageNodeLabelRule) {
		// Simple wrapper: delegate to absolute for now; server is authoritative
		return this.getPageTree(pageNodeLabelRule);
	}

	queryNodes(labelRule) {
		const msg = new HTTP3TreeMessage().setRequestId(this._nextRequestId()).encodeGetQueryNodesRequest(String(labelRule));
		const waitP = this._enqueue(msg, 'vecTree');
		return this.blockingMode_ ? this.awaitVectorTreeResponse(waitP) : [];
	}

	relativeQueryNodes(_node, labelRule) { return this.queryNodes(labelRule); }

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
		const msg = new HTTP3TreeMessage().setRequestId(this._nextRequestId()).encodeGetFullTreeRequest();
		const waitP = this._enqueue(msg, 'vecTree');
		return this.blockingMode_ ? this.awaitVectorTreeResponse(waitP) : [];
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
		let resolved = false;
		const finish = (value) => { resolved = true; if (waiter) { this.waits_.delete(reqId); waiter.resolve(value); } };
		const completeBool = () => {
			const bytes = decodeFromChunks(http3TreeMessage.responseChunks);
			const s = new TextDecoder('utf-8').decode(bytes).trim();
			finish(s.length === 0 ? true : /^(true|1)$/i.test(s));
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
					const bytes = decodeFromChunks(http3TreeMessage.responseChunks);
					// Try Maybe<TreeNode> first, then TreeNode
					let nodeMaybe = null;
					try {
						const maybe = decode_maybe_tree_node(bytes, 0);
						nodeMaybe = maybe.value;
					} catch (_) {
						// Fallback to plain TreeNode
						const one = decode_tree_node(bytes, 0);
						nodeMaybe = Just(one.value);
					}
					if (nodeMaybe && nodeMaybe.isJust && nodeMaybe.isJust()) {
						const node = nodeMaybe.getOrElse(null);
						this.staticNode_ = nodeMaybe;
						try { this.localBackend_.upsertNode([node]); } catch (_) {}
					}
				} catch (_) {
					// ignore parse errors
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
				finish(vec);
				break;
			}
			case WWATP_SIGNAL.SIGNAL_WWATP_APPLY_TRANSACTION_RESPONSE: {
				completeBool();
				break;
			}
			case WWATP_SIGNAL.SIGNAL_WWATP_GET_JOURNAL_RESPONSE: {
				const notifications = http3TreeMessage.decodeGetJournalResponse();
				for (const n of notifications) this.processOneNotification(n);
				if (notifications.length) this.updateLastNotification(notifications[notifications.length - 1]);
				this.journalRequestWaiting_ = false;
				if (waiter) finish(true);
				break;
			}
			default: {
				// Unknown: resolve false
				if (waiter) finish(false);
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

	// ---- Mutable page tree helpers (minimal) ----
	requestFullTreeSync() {
		const msg = new HTTP3TreeMessage().setRequestId(this._nextRequestId()).encodeGetFullTreeRequest();
		return this._enqueue(msg, this.blockingMode_ ? 'vecTree' : null);
	}
	setMutablePageTreeLabelRule(label = 'MutableNodes') { this.mutablePageTreeLabelRule_ = String(label || 'MutableNodes'); }
	getMutablePageTree() { return this.getPageTree(this.mutablePageTreeLabelRule_); }

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
