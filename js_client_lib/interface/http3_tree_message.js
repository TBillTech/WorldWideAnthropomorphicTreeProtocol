// HTTP3TreeMessage – request/response message builder for WWATP.
// Minimal initial implementation covering core state and chunk handling.

import {
	chunkToWire,
	chunkFromWire,
	encodeChunks_label,
	canDecodeChunks_label,
	encodeChunks_MaybeTreeNode,
	encodeChunks_VectorTreeNode,
	encodeChunks_NewNodeVersion,
	encodeChunks_SubTransaction,
	encodeChunks_Transaction,
	encodeChunks_SequentialNotification,
	encodeChunks_VectorSequentialNotification,
	decodeChunks_MaybeTreeNode,
	decodeChunks_VectorTreeNode,
	decodeChunks_NewNodeVersion,
	decodeChunks_SubTransaction,
	decodeChunks_Transaction,
	decodeChunks_SequentialNotification,
	decodeChunks_VectorSequentialNotification,
	canDecodeChunks_MaybeTreeNode,
	canDecodeChunks_VectorTreeNode,
	canDecodeChunks_NewNodeVersion,
	canDecodeChunks_SubTransaction,
	canDecodeChunks_Transaction,
	canDecodeChunks_SequentialNotification,
	canDecodeChunks_VectorSequentialNotification,
	encodeToChunks,
	WWATP_SIGNAL,
} from './http3_tree_message_helpers.js';

import { Nothing, Just } from './maybe.js';

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
	pushResponseChunk(chunk) {
		// Track the response signal from the incoming chunk (payload/signals carry it)
		try { if (chunk?.header && typeof chunk.signal === 'number') this.signal = chunk.signal & 0xff; } catch {}
		// Only payload chunks contribute to decodability of the response body; still store all for parity
		this.responseChunks.push(chunk);
		// Update responseComplete when we have enough chunks to decode the response for known signals
		const endIdx = this._responseDecodableEndIndex();
		if (endIdx !== null && endIdx <= this.responseChunks.length) {
			this.responseComplete = true;
			// Ad-hoc diagnostic: log when responseComplete becomes true
			try { console.info('[HTTP3TreeMessage] responseComplete.set', { rid: this.requestId, sig: this.signal, chunks: this.responseChunks.length, endIdx }); } catch {}
		}
		return this;
	}

	// Determine if responseChunks constitute a full decodable response body starting at index 0.
	// Returns nextChunk index when decodable, or null if insufficient.
	_responseDecodableEndIndex() {
		const sig = this.signal >>> 0;
		// Map response signals to their canDecode functions
		switch (sig) {
			case WWATP_SIGNAL.SIGNAL_WWATP_GET_NODE_RESPONSE:
				return canDecodeChunks_MaybeTreeNode(0, this.responseChunks);
			case WWATP_SIGNAL.SIGNAL_WWATP_GET_PAGE_TREE_RESPONSE:
			case WWATP_SIGNAL.SIGNAL_WWATP_QUERY_NODES_RESPONSE:
			case WWATP_SIGNAL.SIGNAL_WWATP_GET_FULL_TREE_RESPONSE:
				return canDecodeChunks_VectorTreeNode(0, this.responseChunks);
			case WWATP_SIGNAL.SIGNAL_WWATP_GET_JOURNAL_RESPONSE:
				return canDecodeChunks_VectorSequentialNotification(0, this.responseChunks);
			case WWATP_SIGNAL.SIGNAL_WWATP_APPLY_TRANSACTION_RESPONSE:
			case WWATP_SIGNAL.SIGNAL_WWATP_OPEN_TRANSACTION_LAYER_RESPONSE:
			case WWATP_SIGNAL.SIGNAL_WWATP_CLOSE_TRANSACTION_LAYERS_RESPONSE:
			case WWATP_SIGNAL.SIGNAL_WWATP_REGISTER_LISTENER_RESPONSE:
			case WWATP_SIGNAL.SIGNAL_WWATP_DEREGISTER_LISTENER_RESPONSE:
			case WWATP_SIGNAL.SIGNAL_WWATP_NOTIFY_LISTENERS_RESPONSE:
			case WWATP_SIGNAL.SIGNAL_WWATP_PROCESS_NOTIFICATION_RESPONSE:
			case WWATP_SIGNAL.SIGNAL_WWATP_UPSERT_NODE_RESPONSE:
				// Simple label-encoded boolean string; a single payload chunk is sufficient
				return canDecodeChunks_label(0, this.responseChunks);
			default:
				// Unknown or request signals: do not mark complete based on payload accumulation
				return null;
		}
	}

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
		this.requestComplete = true;
		return this;
	}
	decodeGetNodeResponse() {
		// Use chunk-based decoding
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
		const chunk = this.responseChunks[0];
		if (!chunk || !chunk.payload || chunk.payload.byteLength === 0) return true;
		return chunk.payload[0] !== 0;
	}

	// deleteNode(labelRule)
	encodeDeleteNodeRequest(labelRule) {
		this.requestChunks = encodeChunks_label(this.requestId, WWATP_SIGNAL.SIGNAL_WWATP_DELETE_NODE_REQUEST, String(labelRule));
		this.isInitialized = true;
		this.requestComplete = true;
		return this;
	}
	decodeDeleteNodeResponse() {
		const chunk = this.responseChunks[0];
		if (!chunk || !chunk.payload || chunk.payload.byteLength === 0) return true;
		return chunk.payload[0] !== 0;
	}

	// getPageTree(page_node_label_rule)
	encodeGetPageTreeRequest(labelRule) {
		this.requestChunks = encodeChunks_label(this.requestId, WWATP_SIGNAL.SIGNAL_WWATP_GET_PAGE_TREE_REQUEST, String(labelRule));
		this.isInitialized = true;
		this.requestComplete = true;
		return this;
	}
	decodeGetPageTreeResponse() {
		return decodeChunks_VectorTreeNode(0, this.responseChunks).value;
	}

	// queryNodes(labelRule)
	encodeGetQueryNodesRequest(labelRule) {
		this.requestChunks = encodeChunks_label(this.requestId, WWATP_SIGNAL.SIGNAL_WWATP_QUERY_NODES_REQUEST, String(labelRule));
		this.isInitialized = true;
		this.requestComplete = true;
		return this;
	}
	decodeGetQueryNodesResponse() {
		return decodeChunks_VectorTreeNode(0, this.responseChunks).value;
	}

	// openTransactionLayer(TreeNode)
	encodeOpenTransactionLayerRequest(node) {
		this.requestChunks = encodeChunks_MaybeTreeNode(this.requestId, WWATP_SIGNAL.SIGNAL_WWATP_OPEN_TRANSACTION_LAYER_REQUEST, Just(node));
		this.isInitialized = true;
		this.requestComplete = true;
		return this;
	}
	decodeOpenTransactionLayerResponse() {
		const chunk = this.responseChunks[0];
		if (!chunk || !chunk.payload || chunk.payload.byteLength === 0) return true;
		return chunk.payload[0] !== 0;
	}

	// closeTransactionLayers()
	encodeCloseTransactionLayersRequest() {
		// No payload, use encodeToChunks to set signal
		this.requestChunks = encodeToChunks(new Uint8Array(0), { signal: WWATP_SIGNAL.SIGNAL_WWATP_CLOSE_TRANSACTION_LAYERS_REQUEST, requestId: this.requestId });
		this.isInitialized = true;
		this.requestComplete = true;
		return this;
	}
	decodeCloseTransactionLayersResponse() {
		const chunk = this.responseChunks[0];
		if (!chunk || !chunk.payload || chunk.payload.byteLength === 0) return true;
		return chunk.payload[0] !== 0;
	}

	// applyTransaction(Transaction)
	encodeApplyTransactionRequest(transaction) {
		this.requestChunks = encodeChunks_Transaction(this.requestId, WWATP_SIGNAL.SIGNAL_WWATP_APPLY_TRANSACTION_REQUEST, transaction);
		this.isInitialized = true;
		this.requestComplete = true;
		return this;
	}
	decodeApplyTransactionResponse() {
		const chunk = this.responseChunks[0];
		if (!chunk || !chunk.payload || chunk.payload.byteLength === 0) return true;
		return chunk.payload[0] !== 0;
	}

	// getFullTree()
	encodeGetFullTreeRequest() {
		// No payload, use encodeToChunks to set signal
		this.requestChunks = encodeToChunks(new Uint8Array(0), { signal: WWATP_SIGNAL.SIGNAL_WWATP_GET_FULL_TREE_REQUEST, requestId: this.requestId });
		this.isInitialized = true;
		this.requestComplete = true;
		return this;
	}
	decodeGetFullTreeResponse() {
		return decodeChunks_VectorTreeNode(0, this.responseChunks).value;
	}

	// registerNodeListener(listenerName, labelRule, childNotify)
	encodeRegisterNodeListenerRequest(listenerName, labelRule, childNotify) {
		// Use chunk-based label encoding: "listenerName labelRule childNotify"
		const label = `${listenerName} ${labelRule} ${childNotify ? 1 : 0}`;
		this.requestChunks = encodeChunks_label(this.requestId, WWATP_SIGNAL.SIGNAL_WWATP_REGISTER_LISTENER_REQUEST, label);
		this.isInitialized = true;
		this.requestComplete = true;
		return this;
	}
	decodeRegisterNodeListenerResponse() {
		const chunk = this.responseChunks[0];
		if (!chunk || !chunk.payload || chunk.payload.byteLength === 0) return true;
		return chunk.payload[0] !== 0;
	}

	// deregisterNodeListener(listenerName, labelRule)
	encodeDeregisterNodeListenerRequest(listenerName, labelRule) {
		// Use chunk-based label encoding: "listenerName labelRule"
		const label = `${listenerName} ${labelRule}`;
		this.requestChunks = encodeChunks_label(this.requestId, WWATP_SIGNAL.SIGNAL_WWATP_DEREGISTER_LISTENER_REQUEST, label);
		this.isInitialized = true;
		this.requestComplete = true;
		return this;
	}
	decodeDeregisterNodeListenerResponse() {
		const chunk = this.responseChunks[0];
		if (!chunk || !chunk.payload || chunk.payload.byteLength === 0) return true;
		return chunk.payload[0] !== 0;
	}

	// notifyListeners(labelRule, maybeNode)
	encodeNotifyListenersRequest(labelRule, maybeNode) {
		// C++: encode_label(labelRule) + encodeChunks_MaybeTreeNode
		const labelChunks = encodeChunks_label(this.requestId, WWATP_SIGNAL.SIGNAL_WWATP_NOTIFY_LISTENERS_REQUEST, String(labelRule));
		const maybeChunks = encodeChunks_MaybeTreeNode(this.requestId, WWATP_SIGNAL.SIGNAL_WWATP_NOTIFY_LISTENERS_REQUEST, maybeNode);
		this.requestChunks = [...labelChunks, ...maybeChunks];
		this.isInitialized = true;
		this.requestComplete = true;
		return this;
	}
	decodeNotifyListenersResponse() {
		const chunk = this.responseChunks[0];
		if (!chunk || !chunk.payload || chunk.payload.byteLength === 0) return true;
		return chunk.payload[0] !== 0;
	}

	// processNotification()
	encodeProcessNotificationRequest() {
		this.requestChunks = encodeToChunks(new Uint8Array(0), { signal: WWATP_SIGNAL.SIGNAL_WWATP_PROCESS_NOTIFICATION_REQUEST, requestId: this.requestId });
		this.isInitialized = true;
		this.requestComplete = true;
		return this;
	}
	decodeProcessNotificationResponse() {
		const chunk = this.responseChunks[0];
		if (!chunk || !chunk.payload || chunk.payload.byteLength === 0) return true;
		return chunk.payload[0] !== 0;
	}

	// getJournal(last_notification: SequentialNotification)
	encodeGetJournalRequest(lastNotification) {
		// Build chunk-based encoding to match C++: long_string("<count> <label>") + Maybe<TreeNode>
		const sanitized = {
			signalCount: (lastNotification?.signalCount >>> 0) || 0,
			notification: { labelRule: '', maybeNode: Nothing },
		};
		this.requestChunks = encodeChunks_SequentialNotification(this.requestId, WWATP_SIGNAL.SIGNAL_WWATP_GET_JOURNAL_REQUEST, sanitized);
		this.isInitialized = true;
		this.requestComplete = true;
		return this;
	}
	decodeGetJournalResponse() {
        return decodeChunks_VectorSequentialNotification(0, this.responseChunks).value;
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

