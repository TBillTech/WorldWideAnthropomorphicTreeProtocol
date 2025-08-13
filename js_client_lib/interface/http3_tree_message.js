// HTTP3TreeMessage – request/response message builder for WWATP.
// Minimal initial implementation covering core state and chunk handling.

import {
	SpanChunk,
	PayloadChunkHeader,
	SignalChunkHeader,
	chunkToWire,
	chunkFromWire,
	encode_transaction,
	decode_transaction,
	encode_sequential_notification,
	decode_sequential_notification,
	encode_vec_sequential_notification,
	decode_vec_sequential_notification,
	encodeToChunks,
	decodeFromChunks,
	WWATP_SIGNAL,
	SIGNAL_TYPE,
	// Chunk-based C++-parity helpers
	encodeChunks_label,
	decodeChunks_label,
	encodeChunks_MaybeTreeNode,
	decodeChunks_MaybeTreeNode,
	encodeChunks_VectorTreeNode,
	decodeChunks_VectorTreeNode,
} from './http3_tree_message_helpers.js';

import { Maybe, Nothing, Just } from './maybe.js';
import { TreeNode } from './tree_node.js';

export default class HTTP3TreeMessage {
	constructor() {
		this.requestId = 0;
		this.signal = 0;
		this.isInitialized = false;
		this.isJournalRequest = false;
		this.requestComplete = false;
		this.responseComplete = false;
		this.processingFinished = false;
		this.requestChunks = [];
		this.responseChunks = [];
	}

	reset() {
		this.isInitialized = false;
		this.isJournalRequest = false;
		this.requestComplete = false;
		this.responseComplete = false;
		this.processingFinished = false;
		this.requestChunks = [];
		this.responseChunks = [];
		this.requestId = 0;
		this.signal = 0;
	}

	setRequestId(id) { this.requestId = id & 0xffff; return this; }
	setSignal(sig) { this.signal = sig & 0xff; return this; }
	setJournalRequest(flag) { this.isJournalRequest = !!flag; return this; }

	hasNextRequestChunk() { return this.requestChunks.length > 0; }
	popRequestChunk() { return this.requestChunks.shift(); }
	hasNextResponseChunk() { return this.responseChunks.length > 0; }
	popResponseChunk() { return this.responseChunks.shift(); }
	pushResponseChunk(chunk) { this.responseChunks.push(chunk); return this; }

	// Utility: push raw wire bytes (e.g., from transport) into response as a chunk
	pushResponseBytes(bytes) {
		const { chunk } = chunkFromWire(bytes);
		this.pushResponseChunk(chunk);
	}

	// Encode simple requests/responses. We'll expand coverage incrementally.
	// getNode(labelRule)
	encodeGetNodeRequest(labelRule) {
	this.requestChunks = encodeChunks_label(this.requestId, WWATP_SIGNAL.SIGNAL_WWATP_GET_NODE_REQUEST, String(labelRule));
		this.isInitialized = true;
		this.requestComplete = true; // single-shot request
		return this;
	}
	decodeGetNodeResponse() {
	const { value } = decodeChunks_MaybeTreeNode(0, this.responseChunks);
	return value;
	}

	// upsertNode(Vector<TreeNode>)
	encodeUpsertNodeRequest(nodes) {
	this.requestChunks = encodeChunks_VectorTreeNode(this.requestId, WWATP_SIGNAL.SIGNAL_WWATP_UPSERT_NODE_REQUEST, nodes);
		this.isInitialized = true;
		this.requestComplete = true;
		return this;
	}
	decodeUpsertNodeResponse() {
		const bytes = decodeFromChunks(this.responseChunks);
		// C++ encodes bool responses as label strings ("true"/"false")
		const s = new TextDecoder('utf-8').decode(bytes).trim();
		if (s.length === 0) return true;
		return /^(true|1)$/i.test(s);
	}

	// deleteNode(labelRule)
	encodeDeleteNodeRequest(labelRule) {
	this.requestChunks = encodeChunks_label(this.requestId, WWATP_SIGNAL.SIGNAL_WWATP_DELETE_NODE_REQUEST, String(labelRule));
		this.isInitialized = true;
		this.requestComplete = true;
		return this;
	}
	decodeDeleteNodeResponse() {
		const bytes = decodeFromChunks(this.responseChunks);
		const s = new TextDecoder('utf-8').decode(bytes).trim();
		if (s.length === 0) return true;
		return /^(true|1)$/i.test(s);
	}

	// getPageTree(page_node_label_rule)
	encodeGetPageTreeRequest(labelRule) {
	this.requestChunks = encodeChunks_label(this.requestId, WWATP_SIGNAL.SIGNAL_WWATP_GET_PAGE_TREE_REQUEST, String(labelRule));
		this.isInitialized = true;
		this.requestComplete = true;
		return this;
	}
	decodeGetPageTreeResponse() {
	const { value } = decodeChunks_VectorTreeNode(0, this.responseChunks);
	return value;
	}

	// queryNodes(labelRule)
	encodeGetQueryNodesRequest(labelRule) {
	this.requestChunks = encodeChunks_label(this.requestId, WWATP_SIGNAL.SIGNAL_WWATP_QUERY_NODES_REQUEST, String(labelRule));
		this.isInitialized = true;
		this.requestComplete = true;
		return this;
	}
	decodeGetQueryNodesResponse() {
	const { value } = decodeChunks_VectorTreeNode(0, this.responseChunks);
	return value;
	}

	// openTransactionLayer(TreeNode)
	encodeOpenTransactionLayerRequest(node) {
		// encode as Maybe<TreeNode> with Just(node) for parity with C++
	this.requestChunks = encodeChunks_MaybeTreeNode(this.requestId, WWATP_SIGNAL.SIGNAL_WWATP_OPEN_TRANSACTION_LAYER_REQUEST, Just(node));
		this.isInitialized = true;
		this.requestComplete = true;
		return this;
	}
	decodeOpenTransactionLayerResponse() {
		const bytes = decodeFromChunks(this.responseChunks);
		const s = new TextDecoder('utf-8').decode(bytes).trim();
		if (s.length === 0) return true;
		return /^(true|1)$/i.test(s);
	}

	// closeTransactionLayers()
	encodeCloseTransactionLayersRequest() {
		// No payload
		this.requestChunks = encodeToChunks(new Uint8Array(0), { signal: WWATP_SIGNAL.SIGNAL_WWATP_CLOSE_TRANSACTION_LAYERS_REQUEST, requestId: this.requestId });
		this.isInitialized = true;
		this.requestComplete = true;
		return this;
	}
	decodeCloseTransactionLayersResponse() {
		const bytes = decodeFromChunks(this.responseChunks);
		const s = new TextDecoder('utf-8').decode(bytes).trim();
		if (s.length === 0) return true;
		return /^(true|1)$/i.test(s);
	}

	// applyTransaction(Transaction)
	encodeApplyTransactionRequest(transaction) {
		const payload = encode_transaction(transaction);
		this.requestChunks = encodeToChunks(payload, { signal: WWATP_SIGNAL.SIGNAL_WWATP_APPLY_TRANSACTION_REQUEST, requestId: this.requestId });
		this.isInitialized = true;
		this.requestComplete = true;
		return this;
	}
	decodeApplyTransactionResponse() {
		const bytes = decodeFromChunks(this.responseChunks);
		const s = new TextDecoder('utf-8').decode(bytes).trim();
		if (s.length === 0) return true;
		return /^(true|1)$/i.test(s);
	}

	// getFullTree()
	encodeGetFullTreeRequest() {
	this.requestChunks = encodeToChunks(new Uint8Array(0), { signal: WWATP_SIGNAL.SIGNAL_WWATP_GET_FULL_TREE_REQUEST, requestId: this.requestId });
		this.isInitialized = true;
		this.requestComplete = true;
		return this;
	}
	decodeGetFullTreeResponse() {
	const { value } = decodeChunks_VectorTreeNode(0, this.responseChunks);
	return value;
	}

	// registerNodeListener(listenerName, labelRule, childNotify)
	encodeRegisterNodeListenerRequest(listenerName, labelRule, childNotify) {
		// Reuse label chunk encoder twice then a tiny payload for bool
		const chunks = [
			...encodeChunks_label(this.requestId, WWATP_SIGNAL.SIGNAL_WWATP_REGISTER_LISTENER_REQUEST, String(listenerName)),
			...encodeChunks_label(this.requestId, WWATP_SIGNAL.SIGNAL_WWATP_REGISTER_LISTENER_REQUEST, String(labelRule)),
			new SpanChunk(new PayloadChunkHeader(this.requestId, WWATP_SIGNAL.SIGNAL_WWATP_REGISTER_LISTENER_REQUEST, 1), new Uint8Array([childNotify ? 1 : 0]))
		];
		this.requestChunks = chunks;
		this.isInitialized = true;
		this.requestComplete = true;
		return this;
	}
	decodeRegisterNodeListenerResponse() {
		const bytes = decodeFromChunks(this.responseChunks);
		const s = new TextDecoder('utf-8').decode(bytes).trim();
		if (s.length === 0) return true;
		return /^(true|1)$/i.test(s);
	}

	// deregisterNodeListener(listenerName, labelRule)
	encodeDeregisterNodeListenerRequest(listenerName, labelRule) {
		const chunks = [
			...encodeChunks_label(this.requestId, WWATP_SIGNAL.SIGNAL_WWATP_DEREGISTER_LISTENER_REQUEST, String(listenerName)),
			...encodeChunks_label(this.requestId, WWATP_SIGNAL.SIGNAL_WWATP_DEREGISTER_LISTENER_REQUEST, String(labelRule))
		];
		this.requestChunks = chunks;
		this.isInitialized = true;
		this.requestComplete = true;
		return this;
	}
	decodeDeregisterNodeListenerResponse() {
		const bytes = decodeFromChunks(this.responseChunks);
		const s = new TextDecoder('utf-8').decode(bytes).trim();
		if (s.length === 0) return true;
		return /^(true|1)$/i.test(s);
	}

	// notifyListeners(labelRule, maybeNode)
	encodeNotifyListenersRequest(labelRule, maybeNode) {
		const chunks = [
			...encodeChunks_label(this.requestId, WWATP_SIGNAL.SIGNAL_WWATP_NOTIFY_LISTENERS_REQUEST, String(labelRule)),
			...encodeChunks_MaybeTreeNode(this.requestId, WWATP_SIGNAL.SIGNAL_WWATP_NOTIFY_LISTENERS_REQUEST, maybeNode)
		];
		this.requestChunks = chunks;
		this.isInitialized = true;
		this.requestComplete = true;
		return this;
	}
	decodeNotifyListenersResponse() {
		const bytes = decodeFromChunks(this.responseChunks);
		const s = new TextDecoder('utf-8').decode(bytes).trim();
		if (s.length === 0) return true;
		return /^(true|1)$/i.test(s);
	}

	// processNotification()
	encodeProcessNotificationRequest() {
		this.requestChunks = encodeToChunks(new Uint8Array(0), { signal: WWATP_SIGNAL.SIGNAL_WWATP_PROCESS_NOTIFICATION_REQUEST, requestId: this.requestId });
		this.isInitialized = true;
		this.requestComplete = true;
		return this;
	}
	decodeProcessNotificationResponse() {
		const bytes = decodeFromChunks(this.responseChunks);
		const s = new TextDecoder('utf-8').decode(bytes).trim();
		return s.length === 0 ? true : /^(true|1)$/i.test(s);
	}

	// getJournal(last_notification: SequentialNotification)
	encodeGetJournalRequest(lastNotification) {
		// Per C++ tests, when sending to server, drop label and treenode
		const sanitized = {
			signalCount: lastNotification.signalCount >>> 0,
			notification: { labelRule: '', maybeNode: Nothing },
		};
	const payload = encode_sequential_notification(sanitized);
	this.requestChunks = encodeToChunks(payload, { signal: WWATP_SIGNAL.SIGNAL_WWATP_GET_JOURNAL_REQUEST, requestId: this.requestId });
		this.isInitialized = true;
		this.requestComplete = true;
		return this;
	}
	decodeGetJournalResponse() {
		const bytes = decodeFromChunks(this.responseChunks);
		const { value } = decode_vec_sequential_notification(bytes, 0);
		return value;
	}

	// Utilities to access chunks as wire bytes (for transport adapters)
	nextRequestWireBytes() {
		if (!this.hasNextRequestChunk()) return null;
		const chunk = this.requestChunks[0];
		return chunkToWire(chunk);
	}
}

// Local util to concatenate Uint8Array parts
function concatBytes(...parts) {
	let total = 0;
	for (const p of parts) total += p.byteLength;
	const out = new Uint8Array(total);
	let o = 0;
	for (const p of parts) { out.set(p, o); o += p.byteLength; }
	return out;
}

