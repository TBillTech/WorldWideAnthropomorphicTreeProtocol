// HTTP3TreeMessage – request/response message builder for WWATP.
// Minimal initial implementation covering core state and chunk handling.

import {
	SpanChunk,
	PayloadChunkHeader,
	SignalChunkHeader,
	chunkToWire,
	chunkFromWire,
	encode_label,
	encode_vec_tree_node,
	encode_tree_node,
	encode_maybe_tree_node,
	encode_transaction,
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
		// Response carries Maybe<TreeNode>
		// Reuse helper decode by importing locally to avoid circular; inline minimal parsing
		const { decode_maybe_tree_node } = awaitImportHelpers();
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

	// Utilities to access chunks as wire bytes (for transport adapters)
	nextRequestWireBytes() {
		if (!this.hasNextRequestChunk()) return null;
		const chunk = this.requestChunks[0];
		return chunkToWire(chunk);
	}
}

// Small dynamic import shim to avoid circular eager imports for decoders-only path
function awaitImportHelpers() {
	// In Node ESM, dynamic import is async; here we already have the module loaded in bundlers.
	// We'll return the already imported names from the static module system.
	// eslint-disable-next-line no-undef
	return {
		decode_maybe_tree_node: requireLocal('decode_maybe_tree_node'),
	};
}

function requireLocal(name) {
	// Since we are in the same module graph, re-export from helpers via static import is simpler.
	// Provide a minimal mapping:
	switch (name) {
		default: {
			// eslint-disable-next-line no-unused-vars
			const mod = __helpersModule;
			return mod[name];
		}
	}
}

// Build a local module object to expose decoders without circular
import * as __helpersModule from './http3_tree_message_helpers.js';

