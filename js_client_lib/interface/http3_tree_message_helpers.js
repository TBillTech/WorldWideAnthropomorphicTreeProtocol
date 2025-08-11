// HTTP/3 Tree Message helpers – browser-safe, no Buffer usage.
// Provides a minimal chunk model compatible with C++ shared_chunk headers
// and a set of encoders/decoders for WWATP message payloads.

import { Maybe, Just, Nothing } from './maybe.js';
import { TreeNode, TreeNodeVersion } from './tree_node.js';

// ---- Constants (mirror values in C++ shared_chunk.h) ----
export const SIGNAL_TYPE = {
	NONE: 0,
	SIGNAL: 1,
	PAYLOAD: 2,
};

export const WWATP_SIGNAL = {
	SIGNAL_WWATP_REQUEST_CONTINUE: 0x03,
	SIGNAL_WWATP_RESPONSE_CONTINUE: 0x04,
	SIGNAL_WWATP_REQUEST_FINAL: 0x05,
	SIGNAL_WWATP_RESPONSE_FINAL: 0x06,
	SIGNAL_WWATP_GET_NODE_REQUEST: 0x07,
	SIGNAL_WWATP_GET_NODE_RESPONSE: 0x08,
	SIGNAL_WWATP_UPSERT_NODE_REQUEST: 0x09,
	SIGNAL_WWATP_UPSERT_NODE_RESPONSE: 0x0a,
	SIGNAL_WWATP_DELETE_NODE_REQUEST: 0x0b,
	SIGNAL_WWATP_DELETE_NODE_RESPONSE: 0x0c,
	SIGNAL_WWATP_GET_PAGE_TREE_REQUEST: 0x0d,
	SIGNAL_WWATP_GET_PAGE_TREE_RESPONSE: 0x0e,
	SIGNAL_WWATP_QUERY_NODES_REQUEST: 0x0f,
	SIGNAL_WWATP_QUERY_NODES_RESPONSE: 0x10,
	SIGNAL_WWATP_OPEN_TRANSACTION_LAYER_REQUEST: 0x11,
	SIGNAL_WWATP_OPEN_TRANSACTION_LAYER_RESPONSE: 0x12,
	SIGNAL_WWATP_CLOSE_TRANSACTION_LAYERS_REQUEST: 0x13,
	SIGNAL_WWATP_CLOSE_TRANSACTION_LAYERS_RESPONSE: 0x14,
	SIGNAL_WWATP_APPLY_TRANSACTION_REQUEST: 0x15,
	SIGNAL_WWATP_APPLY_TRANSACTION_RESPONSE: 0x16,
	SIGNAL_WWATP_GET_FULL_TREE_REQUEST: 0x17,
	SIGNAL_WWATP_GET_FULL_TREE_RESPONSE: 0x18,
	SIGNAL_WWATP_REGISTER_LISTENER_REQUEST: 0x19,
	SIGNAL_WWATP_REGISTER_LISTENER_RESPONSE: 0x1a,
	SIGNAL_WWATP_DEREGISTER_LISTENER_REQUEST: 0x1b,
	SIGNAL_WWATP_DEREGISTER_LISTENER_RESPONSE: 0x1c,
	SIGNAL_WWATP_NOTIFY_LISTENERS_REQUEST: 0x1d,
	SIGNAL_WWATP_NOTIFY_LISTENERS_RESPONSE: 0x1e,
	SIGNAL_WWATP_PROCESS_NOTIFICATION_REQUEST: 0x1f,
	SIGNAL_WWATP_PROCESS_NOTIFICATION_RESPONSE: 0x20,
	SIGNAL_WWATP_GET_JOURNAL_REQUEST: 0x21,
	SIGNAL_WWATP_GET_JOURNAL_RESPONSE: 0x22,
};

export const DEFAULT_CHUNK_SIZE = 1200; // matches UDPChunk size in C++

// ---- Chunk headers (JS representations) ----
export class NoChunkHeader {
	constructor() {
		this.signal_type = SIGNAL_TYPE.NONE;
	}
	getWireSize() { return 1; }
}

export class SignalChunkHeader {
	constructor(request_id = 0, signal = 0) {
		this.signal_type = SIGNAL_TYPE.SIGNAL;
		this.signal = signal & 0xff;
		this.request_id = request_id & 0xffff;
	}
	getWireSize() { return 1 + 1 + 2; }
}

export class PayloadChunkHeader {
	constructor(request_id = 0, signal = 0, data_length = 0) {
		this.signal_type = SIGNAL_TYPE.PAYLOAD;
		this.signal = signal & 0xff;
		this.request_id = request_id & 0xffff;
		this.data_length = data_length & 0xffff;
	}
	getWireSize() { return 1 + 1 + 2 + 2; }
}

// ---- SpanChunk: a simplified shared_span equivalent ----
export class SpanChunk {
	constructor(header, payload = new Uint8Array(0)) {
		this.header = header; // NoChunkHeader | SignalChunkHeader | PayloadChunkHeader
		this.payload = payload instanceof Uint8Array ? payload : new Uint8Array(payload || 0);
		if (this.header instanceof PayloadChunkHeader) {
			// keep data_length in sync
			this.header.data_length = this.payload.byteLength & 0xffff;
		}
	}
	get signalType() { return this.header?.signal_type ?? 0; }
	get wireSize() {
		return this.header.getWireSize() + (this.header instanceof PayloadChunkHeader ? this.payload.byteLength : 0);
	}
	get signal() {
		if (this.signalType === SIGNAL_TYPE.NONE) throw new Error('No signal for NONE header');
		return this.header.signal & 0xff;
	}
	set signal(v) {
		if (this.signalType === SIGNAL_TYPE.NONE) throw new Error('No signal for NONE header');
		this.header.signal = v & 0xff;
	}
	get requestId() {
		if (this.signalType === SIGNAL_TYPE.NONE) throw new Error('No requestId for NONE header');
		return this.header.request_id & 0xffff;
	}
	set requestId(v) {
		if (this.signalType === SIGNAL_TYPE.NONE) throw new Error('No requestId for NONE header');
		this.header.request_id = v & 0xffff;
	}
}

// ---- Wire encode/decode for a single SpanChunk ----
export function chunkToWire(chunk) {
	const h = chunk.header;
	if (h instanceof NoChunkHeader) {
		const out = new Uint8Array(1);
		out[0] = SIGNAL_TYPE.NONE;
		return out;
	}
	if (h instanceof SignalChunkHeader) {
		const out = new Uint8Array(1 + 1 + 2);
		const dv = new DataView(out.buffer);
		dv.setUint8(0, SIGNAL_TYPE.SIGNAL);
		dv.setUint8(1, h.signal & 0xff);
		dv.setUint16(2, h.request_id & 0xffff, true);
		return out;
	}
	if (h instanceof PayloadChunkHeader) {
		const out = new Uint8Array(1 + 1 + 2 + 2 + chunk.payload.byteLength);
		const dv = new DataView(out.buffer);
		dv.setUint8(0, SIGNAL_TYPE.PAYLOAD);
		dv.setUint8(1, h.signal & 0xff);
		dv.setUint16(2, h.request_id & 0xffff, true);
		dv.setUint16(4, (chunk.payload.byteLength & 0xffff), true);
		out.set(chunk.payload, 6);
		return out;
	}
	throw new Error('Unknown chunk header type');
}

export function chunkFromWire(bytes) {
	if (!(bytes instanceof Uint8Array)) throw new TypeError('bytes must be Uint8Array');
	if (bytes.byteLength < 1) throw new Error('Insufficient bytes');
	const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
	const st = dv.getUint8(0);
	if (st === SIGNAL_TYPE.NONE) {
		return { chunk: new SpanChunk(new NoChunkHeader()), read: 1 };
	}
	if (st === SIGNAL_TYPE.SIGNAL) {
		if (bytes.byteLength < 4) throw new Error('Incomplete signal_chunk_header');
		const sig = dv.getUint8(1);
		const rid = dv.getUint16(2, true);
		return { chunk: new SpanChunk(new SignalChunkHeader(rid, sig)), read: 4 };
	}
	if (st === SIGNAL_TYPE.PAYLOAD) {
		if (bytes.byteLength < 6) throw new Error('Incomplete payload_chunk_header');
		const sig = dv.getUint8(1);
		const rid = dv.getUint16(2, true);
		const len = dv.getUint16(4, true);
		if (bytes.byteLength < 6 + len) throw new Error('Incomplete payload');
		const payload = bytes.slice(6, 6 + len);
		return { chunk: new SpanChunk(new PayloadChunkHeader(rid, sig, len), payload), read: 6 + len };
	}
	throw new Error('Unknown signal_type');
}

// Split payload bytes into multiple Payload chunks with the given signal and requestId.
export function flattenWithSignal(dataBytes, signal, requestId, maxChunkSize = DEFAULT_CHUNK_SIZE) {
	const out = [];
	const headerSize = 6; // payload header size on wire
	const maxPayload = Math.max(0, maxChunkSize - headerSize);
	for (let ofs = 0; ofs < dataBytes.byteLength; ofs += maxPayload) {
		const slice = dataBytes.slice(ofs, Math.min(ofs + maxPayload, dataBytes.byteLength));
		out.push(new SpanChunk(new PayloadChunkHeader(requestId, signal, slice.byteLength), slice));
	}
	if (out.length === 0) out.push(new SpanChunk(new PayloadChunkHeader(requestId, signal, 0), new Uint8Array(0)));
	return out;
}

// Utility to concatenate payload bytes from a list of chunks
export function collectPayloadBytes(chunks) {
	let total = 0;
	for (const c of chunks) if (c.signalType === SIGNAL_TYPE.PAYLOAD) total += c.payload.byteLength;
	const out = new Uint8Array(total);
	let o = 0;
	for (const c of chunks) {
		if (c.signalType !== SIGNAL_TYPE.PAYLOAD) continue;
		out.set(c.payload, o); o += c.payload.byteLength;
	}
	return out;
}

// ---- Binary encoding helpers ----
function utf8Encode(str) { return new TextEncoder().encode(String(str)); }
function utf8Decode(u8) { return new TextDecoder('utf-8', { fatal: false }).decode(u8); }
function u8Concat(...parts) {
	const total = parts.reduce((s, p) => s + p.byteLength, 0);
	const out = new Uint8Array(total);
	let o = 0;
	for (const p of parts) { out.set(p, o); o += p.byteLength; }
	return out;
}

// label: uint8 length + bytes
export function encode_label(label) {
	const b = utf8Encode(label);
	if (b.byteLength > 255) throw new Error('label too long');
	const out = new Uint8Array(1 + b.byteLength);
	out[0] = b.byteLength & 0xff;
	out.set(b, 1);
	return out;
}
export function can_decode_label(bytes, offset = 0) {
	if (bytes.byteLength < offset + 1) return false;
	const len = bytes[offset];
	return bytes.byteLength >= offset + 1 + len;
}
export function decode_label(bytes, offset = 0) {
	if (!can_decode_label(bytes, offset)) throw new Error('insufficient bytes for label');
	const len = bytes[offset];
	const value = utf8Decode(bytes.slice(offset + 1, offset + 1 + len));
	return { value, read: 1 + len };
}

// long_string: uint32 le length + bytes
export function encode_long_string(str) {
	const b = utf8Encode(str);
	const out = new Uint8Array(4 + b.byteLength);
	const dv = new DataView(out.buffer);
	dv.setUint32(0, b.byteLength >>> 0, true);
	out.set(b, 4);
	return out;
}
export function can_decode_long_string(bytes, offset = 0) {
	if (bytes.byteLength < offset + 4) return false;
	const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
	const len = dv.getUint32(offset, true);
	return bytes.byteLength >= offset + 4 + len;
}
export function decode_long_string(bytes, offset = 0) {
	if (!can_decode_long_string(bytes, offset)) throw new Error('insufficient bytes for long_string');
	const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
	const len = dv.getUint32(offset, true);
	const value = utf8Decode(bytes.slice(offset + 4, offset + 4 + len));
	return { value, read: 4 + len };
}

// TreeNode JSON encoding (browser-safe). propertyData stored as number array.
function treeNodeToPlain(node) {
	return {
		labelRule: node.labelRule,
		description: node.description,
		propertyInfos: node.propertyInfos.map((p) => ({ type: p.type, name: p.name })),
		version: { ...node.version.toJSON() },
		childNames: [...node.childNames],
		propertyData: Array.from(node.propertyData || []),
		queryHowTo: node.queryHowTo.toJSON(),
		qaSequence: node.qaSequence.toJSON(),
	};
}

function plainToTreeNode(obj) {
	const v = TreeNodeVersion.fromJSON(obj.version);
	return new TreeNode({
		labelRule: obj.labelRule,
		description: obj.description,
		propertyInfos: obj.propertyInfos,
		version: v,
		childNames: obj.childNames,
		propertyData: new Uint8Array(obj.propertyData || []),
		queryHowTo: obj.queryHowTo?.kind === 'nothing' ? Nothing : Just(obj.queryHowTo?.value),
		qaSequence: obj.qaSequence?.kind === 'nothing' ? Nothing : Just(obj.qaSequence?.value),
	});
}

export function encode_tree_node(node) {
	const json = JSON.stringify(treeNodeToPlain(node));
	return encode_long_string(json);
}
export function can_decode_tree_node(bytes, offset = 0) {
	return can_decode_long_string(bytes, offset);
}
export function decode_tree_node(bytes, offset = 0) {
	const { value, read } = decode_long_string(bytes, offset);
	const obj = JSON.parse(value);
	return { value: plainToTreeNode(obj), read };
}

// Maybe<TreeNode> : uint8 flag(0/1) + [TreeNode]
export function encode_maybe_tree_node(maybeNode) {
	const isJust = Maybe.isMaybe(maybeNode) ? maybeNode.isJust() : !!maybeNode;
	if (!isJust) return new Uint8Array([0]);
	const node = Maybe.isMaybe(maybeNode) ? maybeNode.value : maybeNode;
	const enc = encode_tree_node(node);
	const out = new Uint8Array(1 + enc.byteLength);
	out[0] = 1;
	out.set(enc, 1);
	return out;
}
export function can_decode_maybe_tree_node(bytes, offset = 0) {
	if (bytes.byteLength < offset + 1) return false;
	if (bytes[offset] === 0) return true; // only flag present
	return can_decode_tree_node(bytes, offset + 1);
}
export function decode_maybe_tree_node(bytes, offset = 0) {
	if (bytes.byteLength < offset + 1) throw new Error('insufficient bytes for maybe');
	const flag = bytes[offset];
	if (flag === 0) return { value: Nothing, read: 1 };
	const { value, read } = decode_tree_node(bytes, offset + 1);
	return { value: Just(value), read: 1 + read };
}

// SequentialNotification: { signalCount:u32, notification: { labelRule, maybeNode } }
export function encode_sequential_notification(sn) {
	const labelBytes = encode_label(sn.notification.labelRule);
	const maybeBytes = encode_maybe_tree_node(sn.notification.maybeNode ?? sn.notification.maybe_node ?? Nothing);
	const out = new Uint8Array(4 + labelBytes.byteLength + maybeBytes.byteLength);
	const dv = new DataView(out.buffer);
	dv.setUint32(0, (sn.signalCount >>> 0) || 0, true);
	out.set(labelBytes, 4);
	out.set(maybeBytes, 4 + labelBytes.byteLength);
	return out;
}
export function can_decode_sequential_notification(bytes, offset = 0) {
	if (bytes.byteLength < offset + 4) return false;
	// we need label, but label is u8 len + bytes, followed by maybe
	if (!can_decode_label(bytes, offset + 4)) return false;
	const { read } = decode_label(bytes, offset + 4);
	return can_decode_maybe_tree_node(bytes, offset + 4 + read);
}
export function decode_sequential_notification(bytes, offset = 0) {
	if (bytes.byteLength < offset + 4) throw new Error('insufficient bytes for seq notif');
	const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
	const signalCount = dv.getUint32(offset, true);
	const lab = decode_label(bytes, offset + 4);
	const mn = decode_maybe_tree_node(bytes, offset + 4 + lab.read);
	return {
		value: { signalCount, notification: { labelRule: lab.value, maybeNode: mn.value } },
		read: 4 + lab.read + mn.read,
	};
}

// Vector<SequentialNotification>
export function encode_vec_sequential_notification(vec) {
	return encode_vec(vec, encode_sequential_notification);
}
export function can_decode_vec_sequential_notification(bytes, offset = 0) {
	return can_decode_vec(bytes, offset, can_decode_sequential_notification, decode_sequential_notification);
}
export function decode_vec_sequential_notification(bytes, offset = 0) {
	return decode_vec(bytes, offset, decode_sequential_notification);
}

// Vector<T> helpers
export function encode_vec(items, encItemFn) {
	if (!Array.isArray(items)) throw new TypeError('encode_vec expects array');
	const encoded = items.map((it) => encItemFn(it));
	const total = encoded.reduce((s, e) => s + e.byteLength, 0);
	const out = new Uint8Array(2 + total);
	const dv = new DataView(out.buffer);
	dv.setUint16(0, items.length & 0xffff, true);
	let o = 2;
	for (const e of encoded) { out.set(e, o); o += e.byteLength; }
	return out;
}
export function can_decode_vec(bytes, offset, canDecItemFn, peekSizeFn) {
	if (bytes.byteLength < offset + 2) return false;
	const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
	const n = dv.getUint16(offset, true);
	let o = offset + 2;
	for (let i = 0; i < n; i++) {
		if (!canDecItemFn(bytes, o)) return false;
		// To advance, we need to know the size to skip; use peekSizeFn by decoding minimally
		const { read } = peekSizeFn(bytes, o);
		o += read;
	}
	return true;
}
export function decode_vec(bytes, offset, decItemFn) {
	if (bytes.byteLength < offset + 2) throw new Error('insufficient bytes for vec');
	const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
	const n = dv.getUint16(offset, true);
	const out = [];
	let o = offset + 2;
	for (let i = 0; i < n; i++) {
		const { value, read } = decItemFn(bytes, o);
		out.push(value);
		o += read;
	}
	return { value: out, read: o - offset };
}

// NewNodeVersion = [Maybe<uint16>, [labelRule, Maybe<TreeNode>]]
export function encode_new_node_version(nnv) {
	const [maybeVer, pair] = nnv;
	const [label, maybeNode] = pair;
	const verFlag = Maybe.isMaybe(maybeVer) ? (maybeVer.isJust() ? 1 : 0) : (maybeVer != null ? 1 : 0);
	const verVal = Maybe.isMaybe(maybeVer) ? (maybeVer.isJust() ? maybeVer.value : 0) : (maybeVer || 0);
	const labelBytes = encode_label(label);
	const nodeBytes = encode_maybe_tree_node(maybeNode);
	const out = new Uint8Array(1 + (verFlag ? 2 : 0) + labelBytes.byteLength + nodeBytes.byteLength);
	let o = 0;
	out[o++] = verFlag;
	if (verFlag) { const dv = new DataView(out.buffer); dv.setUint16(o, verVal & 0xffff, true); o += 2; }
	out.set(labelBytes, o); o += labelBytes.byteLength;
	out.set(nodeBytes, o);
	return out;
}
export function can_decode_new_node_version(bytes, offset = 0) {
	if (bytes.byteLength < offset + 1) return false;
	let o = offset + 1;
	if (bytes[offset] !== 0) o += 2;
	if (!can_decode_label(bytes, o)) return false;
	const { read } = decode_label(bytes, o);
	return can_decode_maybe_tree_node(bytes, o + read);
}
export function decode_new_node_version(bytes, offset = 0) {
	if (bytes.byteLength < offset + 1) throw new Error('insufficient bytes for nnv');
	let o = offset;
	const flag = bytes[o++];
	let maybeVer = Nothing;
	if (flag) { const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength); maybeVer = Just(dv.getUint16(o, true)); o += 2; }
	const lab = decode_label(bytes, o); o += lab.read;
	const mn = decode_maybe_tree_node(bytes, o); o += mn.read;
	return { value: [maybeVer, [lab.value, mn.value]], read: o - offset };
}

// SubTransaction = [NewNodeVersion, NewNodeVersion[]]
export function encode_sub_transaction(st) {
	const [parent, children] = st;
	const parentBytes = encode_new_node_version(parent);
	const childrenBytes = encode_vec(children, encode_new_node_version);
	return u8Concat(parentBytes, childrenBytes);
}
export function can_decode_sub_transaction(bytes, offset = 0) {
	if (!can_decode_new_node_version(bytes, offset)) return false;
	const tmp = decode_new_node_version(bytes, offset);
	return can_decode_vec(bytes, offset + tmp.read, can_decode_new_node_version, decode_new_node_version);
}
export function decode_sub_transaction(bytes, offset = 0) {
	const p = decode_new_node_version(bytes, offset);
	const v = decode_vec(bytes, offset + p.read, decode_new_node_version);
	return { value: [p.value, v.value], read: p.read + v.read };
}

// Transaction = SubTransaction[]
export function encode_transaction(tx) { return encode_vec(tx, encode_sub_transaction); }
export function can_decode_transaction(bytes, offset = 0) { return can_decode_vec(bytes, offset, can_decode_sub_transaction, decode_sub_transaction); }
export function decode_transaction(bytes, offset = 0) { return decode_vec(bytes, offset, decode_sub_transaction); }

// Vector<TreeNode>
export function encode_vec_tree_node(nodes) { return encode_vec(nodes, encode_tree_node); }
export function can_decode_vec_tree_node(bytes, offset = 0) { return can_decode_vec(bytes, offset, can_decode_tree_node, decode_tree_node); }
export function decode_vec_tree_node(bytes, offset = 0) { return decode_vec(bytes, offset, decode_tree_node); }

// Helper to build request/response chunks from encoded payload bytes
export function encodeToChunks(payloadBytes, { signal, requestId, maxChunkSize } = {}) {
	if (typeof signal !== 'number') throw new Error('signal required');
	if (typeof requestId !== 'number') throw new Error('requestId required');
	return flattenWithSignal(payloadBytes, signal, requestId, maxChunkSize);
}

// Decoder from chunks to a contiguous payload buffer
export function decodeFromChunks(chunks) {
	return collectPayloadBytes(chunks);
}

