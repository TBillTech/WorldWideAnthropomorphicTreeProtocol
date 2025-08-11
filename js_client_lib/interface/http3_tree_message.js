// HTTP3TreeMessage – request/response message builder for WWATP.
// Minimal initial implementation covering core state and chunk handling.

import {
	SpanChunk,
	PayloadChunkHeader,
	SignalChunkHeader,
	chunkToWire,
	chunkFromWire,
	encode_label,
	decode_label,
	encode_vec_tree_node,
	decode_vec_tree_node,
	encode_tree_node,
	decode_tree_node,
	encode_maybe_tree_node,
	decode_maybe_tree_node,
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
		const payload = encode_label(labelRule);
		this.requestChunks = encodeToChunks(payload, { signal: WWATP_SIGNAL.SIGNAL_WWATP_GET_NODE_REQUEST, requestId: this.requestId });
		this.isInitialized = true;
		this.requestComplete = true; // single-shot request
		return this;
	}
	decodeGetNodeResponse() {
		const bytes = decodeFromChunks(this.responseChunks);
		const { value } = decode_maybe_tree_node(bytes, 0);
		return value; // Maybe<TreeNode>
	}

	// upsertNode(Vector<TreeNode>)
	encodeUpsertNodeRequest(nodes) {
		const payload = encode_vec_tree_node(nodes);
		this.requestChunks = encodeToChunks(payload, { signal: WWATP_SIGNAL.SIGNAL_WWATP_UPSERT_NODE_REQUEST, requestId: this.requestId });
		this.isInitialized = true;
		this.requestComplete = true;
		return this;
	}
	decodeUpsertNodeResponse() {
		const bytes = decodeFromChunks(this.responseChunks);
		// boolean result encoded as u8 (0/1)
		if (bytes.byteLength < 1) throw new Error('invalid upsert response');
		return bytes[0] !== 0;
	}

	// deleteNode(labelRule)
	encodeDeleteNodeRequest(labelRule) {
		const payload = encode_label(labelRule);
		this.requestChunks = encodeToChunks(payload, { signal: WWATP_SIGNAL.SIGNAL_WWATP_DELETE_NODE_REQUEST, requestId: this.requestId });
		this.isInitialized = true;
		this.requestComplete = true;
		return this;
	}
	decodeDeleteNodeResponse() {
		const bytes = decodeFromChunks(this.responseChunks);
		if (bytes.byteLength < 1) throw new Error('invalid delete response');
		return bytes[0] !== 0;
	}

	// getPageTree(page_node_label_rule)
	encodeGetPageTreeRequest(labelRule) {
		const payload = encode_label(labelRule);
		this.requestChunks = encodeToChunks(payload, { signal: WWATP_SIGNAL.SIGNAL_WWATP_GET_PAGE_TREE_REQUEST, requestId: this.requestId });
		this.isInitialized = true;
		this.requestComplete = true;
		return this;
	}
	decodeGetPageTreeResponse() {
		const bytes = decodeFromChunks(this.responseChunks);
		const { value } = decode_vec_tree_node(bytes, 0);
		return value;
	}

	// queryNodes(labelRule)
	encodeGetQueryNodesRequest(labelRule) {
		const payload = encode_label(labelRule);
		this.requestChunks = encodeToChunks(payload, { signal: WWATP_SIGNAL.SIGNAL_WWATP_QUERY_NODES_REQUEST, requestId: this.requestId });
		this.isInitialized = true;
		this.requestComplete = true;
		return this;
	}
	decodeGetQueryNodesResponse() {
		const bytes = decodeFromChunks(this.responseChunks);
		const { value } = decode_vec_tree_node(bytes, 0);
		return value;
	}

	// openTransactionLayer(TreeNode)
	encodeOpenTransactionLayerRequest(node) {
		// encode as Maybe<TreeNode> with Just(node) for parity with C++
		const payload = encode_maybe_tree_node(Just(node));
		this.requestChunks = encodeToChunks(payload, { signal: WWATP_SIGNAL.SIGNAL_WWATP_OPEN_TRANSACTION_LAYER_REQUEST, requestId: this.requestId });
		this.isInitialized = true;
		this.requestComplete = true;
		return this;
	}
	decodeOpenTransactionLayerResponse() {
		const bytes = decodeFromChunks(this.responseChunks);
		if (bytes.byteLength < 1) throw new Error('invalid openTransactionLayer response');
		return bytes[0] !== 0;
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
		if (bytes.byteLength < 1) throw new Error('invalid closeTransactionLayers response');
		return bytes[0] !== 0;
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
		if (bytes.byteLength < 1) throw new Error('invalid applyTransaction response');
		return bytes[0] !== 0;
	}

	// getFullTree()
	encodeGetFullTreeRequest() {
		this.requestChunks = encodeToChunks(new Uint8Array(0), { signal: WWATP_SIGNAL.SIGNAL_WWATP_GET_FULL_TREE_REQUEST, requestId: this.requestId });
		this.isInitialized = true;
		this.requestComplete = true;
		return this;
	}
	decodeGetFullTreeResponse() {
		const bytes = decodeFromChunks(this.responseChunks);
		const { value } = decode_vec_tree_node(bytes, 0);
		return value;
	}

	// registerNodeListener(listenerName, labelRule, childNotify)
	encodeRegisterNodeListenerRequest(listenerName, labelRule, childNotify) {
		const payload = concatBytes(
			encode_label(listenerName),
			encode_label(labelRule),
			new Uint8Array([childNotify ? 1 : 0])
		);
		this.requestChunks = encodeToChunks(payload, { signal: WWATP_SIGNAL.SIGNAL_WWATP_REGISTER_LISTENER_REQUEST, requestId: this.requestId });
		this.isInitialized = true;
		this.requestComplete = true;
		return this;
	}
	decodeRegisterNodeListenerResponse() {
		const bytes = decodeFromChunks(this.responseChunks);
		if (bytes.byteLength < 1) throw new Error('invalid registerNodeListener response');
		return bytes[0] !== 0;
	}

	// deregisterNodeListener(listenerName, labelRule)
	encodeDeregisterNodeListenerRequest(listenerName, labelRule) {
		const payload = concatBytes(encode_label(listenerName), encode_label(labelRule));
		this.requestChunks = encodeToChunks(payload, { signal: WWATP_SIGNAL.SIGNAL_WWATP_DEREGISTER_LISTENER_REQUEST, requestId: this.requestId });
		this.isInitialized = true;
		this.requestComplete = true;
		return this;
	}
	decodeDeregisterNodeListenerResponse() {
		const bytes = decodeFromChunks(this.responseChunks);
		if (bytes.byteLength < 1) throw new Error('invalid deregisterNodeListener response');
		return bytes[0] !== 0;
	}

	// notifyListeners(labelRule, maybeNode)
	encodeNotifyListenersRequest(labelRule, maybeNode) {
		const payload = concatBytes(encode_label(labelRule), encode_maybe_tree_node(maybeNode));
		this.requestChunks = encodeToChunks(payload, { signal: WWATP_SIGNAL.SIGNAL_WWATP_NOTIFY_LISTENERS_REQUEST, requestId: this.requestId });
		this.isInitialized = true;
		this.requestComplete = true;
		return this;
	}
	decodeNotifyListenersResponse() {
		const bytes = decodeFromChunks(this.responseChunks);
		if (bytes.byteLength < 1) throw new Error('invalid notifyListeners response');
		return bytes[0] !== 0;
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
		// assume empty/true indicates processed; fallback to true if empty
		return bytes.byteLength === 0 ? true : bytes[0] !== 0;
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

