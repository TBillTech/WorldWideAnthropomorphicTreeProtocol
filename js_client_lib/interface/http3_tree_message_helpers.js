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

// Generic transport-level signals (mirror C++ shared_chunk.h)
export const SIGNAL_HEARTBEAT = 0x02;

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

// All Pseudo codes below are for encode functions (decode and can_decode are not shown)
// Pseudo codes focus on two issues:
//   * whether data is written as clear text or binary
//   * when and how many chunks are appended to the encoded list
//   * All encode functions must return a chunkList, which is an array of byte arrays
//   * Each chunk must not exceed 1200 bytes in size
//   * Each chunk must have payload_chunk_header at the beginning

export function encode_label(label) {
	throw new Error('Deprecated legacy helper encode_label: use encodeChunks_label');
}
export function can_decode_label(bytes, offset = 0) {
	throw new Error('Deprecated legacy helper can_decode_label: use canDecodeChunks_label');
}
export function decode_label(bytes, offset = 0) {
	throw new Error('Deprecated legacy helper decode_label: use decodeChunks_label');
}

// long_string: uint32 le length + bytes
export function encode_long_string(str) {
	throw new Error('Deprecated legacy helper encode_long_string: use encodeChunks_long_string');
}
export function can_decode_long_string(bytes, offset = 0) {
	throw new Error('Deprecated legacy helper can_decode_long_string: use canDecodeChunks_long_string');
}
export function decode_long_string(bytes, offset = 0) {
	throw new Error('Deprecated legacy helper decode_long_string: use decodeChunks_long_string');
}

// ---- C++-parity encoders/decoders (chunk-based) ----

// Helpers for 64-bit little-endian length prefix (size_t assumed 8)
function le64Encode(n) {
	// n can be number or bigint
	let v = typeof n === 'bigint' ? n : BigInt(n >>> 0);
	const out = new Uint8Array(8);
	const dv = new DataView(out.buffer);
	// write low 32 then high 32 to keep it simple
	dv.setUint32(0, Number(v & 0xffffffffn) >>> 0, true);
	dv.setUint32(4, Number((v >> 32n) & 0xffffffffn) >>> 0, true);
	return out;
}
function le64Decode(u8, offset = 0) {
	if (u8.byteLength < offset + 8) throw new Error('insufficient for u64');
	const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
	const lo = BigInt(dv.getUint32(offset + 0, true));
	const hi = BigInt(dv.getUint32(offset + 4, true));
	return (hi << 32n) | lo;
}

// Encode a label as a single payload chunk (C++ encode_label)
// Pseudo code for encodeChunks_label
// 1. chunkList encoded starts empty
// 2. Create a chunk with clear text version of string with length
// 3. Append chunk to encoded
// label: uint8 length + bytes
export function encodeChunks_label(requestId, signal, str) {
	const b = utf8Encode(str);
	if (b.byteLength > (DEFAULT_CHUNK_SIZE - 6)) throw new Error('label too long for single chunk');
	return [new SpanChunk(new PayloadChunkHeader(requestId, signal, b.byteLength), b)];
}
export function canDecodeChunks_label(startChunk, chunks) {
	if (startChunk >= chunks.length) return null;
	const c = chunks[startChunk];
	if (!(c.header instanceof PayloadChunkHeader)) return null;
	// Always decodable from one chunk
	return startChunk + 1;
}
export function decodeChunks_label(startChunk, chunks) {
	const next = canDecodeChunks_label(startChunk, chunks);
	if (!next) throw new Error('cannot decode label');
	const c = chunks[startChunk];
	return { nextChunk: next, value: utf8Decode(c.payload) };
}

// Encode a long_string across multiple chunks: 8-byte length + UTF-8, flattened
// Pseudo code for encodeChunks_long_string
// 1. chunkList encoded starts empty
// 2. Create a chunk with binary size_t string length at beginning, filled with string data
// 3. Append chunk to encoded
// 4. Continue adding chunks for the remainder of the long string (chunks must not exceed 1200 in size)
export function encodeChunks_long_string(requestId, signal, str) {
	const b = utf8Encode(str);
	const bytes = u8Concat(le64Encode(b.byteLength), b);
	return flattenWithSignal(bytes, signal, requestId);
}

export function canDecodeChunks_long_string(startChunk, chunks) {
	if (startChunk >= chunks.length) return null;
	// We need to ensure at least 8 bytes exist starting at startChunk across chunk payloads
	let need = 8;
	let idx = startChunk;
	let len = 0n;
	// First, collect 8 bytes
	const tmp = new Uint8Array(8);
	let to = 0;
	while (idx < chunks.length && need > 0) {
		const c = chunks[idx];
		if (!(c.header instanceof PayloadChunkHeader)) return null;
		const take = Math.min(need, c.payload.byteLength);
		tmp.set(c.payload.slice(0, take), to);
		to += take;
		need -= take;
		idx++;
	}
	if (need > 0) return null; // not enough for size yet
	len = le64Decode(tmp, 0);
	// Now ensure total bytes available across chunks cover 8 + len
	const totalNeeded = 8n + len;
	let available = 0n;
	for (let i = startChunk; i < chunks.length; i++) {
		const c = chunks[i]; if (!(c.header instanceof PayloadChunkHeader)) return null;
		available += BigInt(c.payload.byteLength);
		if (available >= totalNeeded) {
			// compute how many chunks consumed to reach boundary
			let acc = 0n;
			for (let j = startChunk; j <= i; j++) {
				acc += BigInt(chunks[j].payload.byteLength);
				if (acc >= totalNeeded) return j + 1;
			}
		}
	}
	return null;
}

export function decodeChunks_long_string(startChunk, chunks) {
	const endChunk = canDecodeChunks_long_string(startChunk, chunks);
	if (!endChunk) throw new Error('cannot decode long_string');
	// Concatenate payload bytes from startChunk to endChunk-1
	const seg = chunks.slice(startChunk, endChunk);
	const bytes = collectPayloadBytes(seg);
	const len = le64Decode(bytes, 0);
	const total = Number(8n + len);
	const strBytes = bytes.slice(8, total);
	return { nextChunk: endChunk, value: utf8Decode(strBytes) };
}

// TreeNode <-> C++ text format (hide_contents on encode)
function maybeStrToText(m) {
	if (!Maybe.isMaybe(m)) return `Just ${String(m)}`;
	return m.isJust() ? `Just ${String(m.value)}` : 'Nothing';
}
function textToMaybe(str) {
	if (str === 'Nothing') return Nothing;
	if (str.startsWith('Just ')) return Just(str.slice(5));
	// Fallback: treat empty as Nothing
	return str ? Just(str) : Nothing;
}

function formatTreeNodeText(node) {
	// Mirrors C++ operator<< with hide_contents: property_data emitted as empty shared_span
	const tn = node instanceof TreeNode ? node : new TreeNode(node || {});
	let out = '';
	out += 'TreeNode(\n';
	out += `label_rule: ${tn.getLabelRule()}\n`;
	const desc = tn.getDescription();
	out += `description ${desc.length} :\n${desc}\n`;
	// property_infos
	out += 'property_infos: [ ';
	const infos = tn.getPropertyInfos();
	for (const { type, name } of infos) {
		out += `${type} (${name}), `;
	}
	out += '], ';
	// version
	const v = tn.getVersion();
	const ap = maybeStrToText(v.authorialProof);
	const au = maybeStrToText(v.authors);
	const rd = maybeStrToText(v.readers);
	const cd = v.collisionDepth && Maybe.isMaybe(v.collisionDepth) ? (v.collisionDepth.isJust() ? `Just ${v.collisionDepth.value}` : 'Nothing') : 'Nothing';
	out += `version: Version: ${v.versionNumber} Max_Version: ${v.maxVersionSequence} Policy: ${v.policy} Authorial_Proof: ${ap} Authors: ${au} Readers: ${rd} Collision_Depth: ${cd} , `;
	// child_names
	out += 'child_names: [ ';
	for (const name of tn.getChildNames()) {
		out += `${name}, `;
	}
	out += '], ';
	// query_how_to
	const qh = tn.getQueryHowTo();
	const qhIsJust = Maybe.isMaybe(qh) && qh.isJust();
	const qhs = qhIsJust ? String(qh.value) : '';
	out += `query_how_to ${qhs.length} :\n${qhs}\n`;
	// qa_sequence
	const qa = tn.getQaSequence();
	const qaIsJust = Maybe.isMaybe(qa) && qa.isJust();
	const qas = qaIsJust ? String(qa.value) : '';
	out += `qa_sequence ${qas.length} :\n${qas}\n`;
	// empty shared_span
	out += 'shared_span( ';
	out += 'signal_type: 0 ';
	out += 'signal_size: 0 ';
	out += 'data_size: 0 ';
	out += 'chunk_data: [\n';
	out += '])\n';
	out += ')\n';
	return out;
}

function parseLengthString(src, cur, expectLabel) {
	// src is JS string; cur is index
	// format: `${label} ${len} :\n` then len chars then `\n`
	const rem = src.slice(cur);
	if (!rem.startsWith(expectLabel + ' ')) throw new Error('bad length string label');
	let i = cur + expectLabel.length + 1;
	// read integer
	let num = '';
	while (i < src.length && /[0-9]/.test(src[i])) { num += src[i++]; }
	if (src.slice(i, i + 2) !== ' :') throw new Error('bad length string sep');
	i += 2;
	if (src[i] !== '\n') throw new Error('expected newline after length label');
	i += 1;
	const n = parseInt(num, 10) || 0;
	const val = src.slice(i, i + n);
	i += n;
	if (src[i] !== '\n') throw new Error('expected newline after content');
	i += 1;
	return { next: i, value: val };
}

function parseTreeNodeText(text) {
	let i = 0;
	function expect(tok) {
		if (text.slice(i, i + tok.length) !== tok) throw new Error(`expected ${tok}`);
		i += tok.length;
	}
	function readUntilNewline(afterPrefix) {
		const nl = text.indexOf('\n', i);
		if (nl < 0) throw new Error('no newline');
		const s = text.slice(i, nl);
		i = nl + 1;
		if (afterPrefix && !s.startsWith(afterPrefix)) throw new Error('prefix mismatch');
		return afterPrefix ? s.slice(afterPrefix.length) : s;
	}
	expect('TreeNode(\n');
	// Read label_rule line, allow empty or any string
	let label_rule = '';
	if (text.slice(i, i + 'label_rule: '.length) === 'label_rule: ') {
		i += 'label_rule: '.length;
		const nl = text.indexOf('\n', i);
		if (nl >= 0) {
			label_rule = text.slice(i, nl);
			i = nl + 1;
		} else {
			// No newline found, treat as empty
			i += 0;
		}
	} else {
		throw new Error('expected label_rule');
	}
	const d = parseLengthString(text, i, 'description'); i = d.next; const description = d.value;
	// property_infos
	expect('property_infos: [ ');
	const propLine = text.slice(i, text.indexOf('], ', i));
	const infos = [];
	if (propLine && propLine.length > 0) {
			const items = propLine.split(',').map(s => s.trim()).filter(Boolean);
		for (const it of items) {
				// format: TYPE (NAME) — NAME may be empty in some cases
				const m = it.match(/^(\S+) \(([^)]*)\)$/);
			if (m) infos.push({ type: m[1], name: m[2] });
		}
	}
	i = text.indexOf('], ', i) + 3;
	// version
	expect('version: ');
	const verEnd = text.indexOf(', ', i);
	const verStr = text.slice(i, verEnd);
	i = verEnd + 2;
	// parse version fields
	const vm = verStr.match(/^Version: (\d+) Max_Version: (\d+) Policy: (\S+) Authorial_Proof: (Nothing|Just\s+\S+) Authors: (Nothing|Just\s+\S+) Readers: (Nothing|Just\s+\S+) Collision_Depth: (Nothing|Just\s+\S+) $/);
	if (!vm) throw new Error('bad version');
	const version = new TreeNodeVersion({
		versionNumber: parseInt(vm[1], 10) || 0,
		maxVersionSequence: parseInt(vm[2], 10) || 256,
		policy: vm[3],
		authorialProof: textToMaybe(vm[4]),
		authors: textToMaybe(vm[5]),
		readers: textToMaybe(vm[6]),
		collisionDepth: vm[7] === 'Nothing' ? Nothing : Just(parseInt(vm[7].slice(5).trim(), 10) || 0),
	});
	// child_names
	expect('child_names: [ ');
	const childLine = text.slice(i, text.indexOf('], ', i));
	const childNames = [];
	if (childLine && childLine.length > 0) {
		const items = childLine.split(',').map(s => s.trim()).filter(Boolean);
		for (const it of items) childNames.push(it);
	}
	i = text.indexOf('], ', i) + 3;
	// query_how_to and qa_sequence
	const qh = parseLengthString(text, i, 'query_how_to'); i = qh.next;
	const qa = parseLengthString(text, i, 'qa_sequence'); i = qa.next;
	// optional maybe_flags line
	let qhFlag = null, qaFlag = null;
	if (text.slice(i, i + 'maybe_flags: '.length) === 'maybe_flags: ') {
		i += 'maybe_flags: '.length;
		// expect format qh=X qa=Y\n
		const nl = text.indexOf('\n', i);
		if (nl < 0) throw new Error('no newline after maybe_flags');
		const line = text.slice(i, nl);
		i = nl + 1;
		const m = line.match(/^qh=(J|N) qa=(J|N)$/);
		if (m) { qhFlag = m[1]; qaFlag = m[2]; }
	}
	// skip empty shared_span text
	const sharedSpanPrefix = 'shared_span( signal_type: 0 signal_size: 0 data_size: 0 chunk_data: [\n])\n';
	if (text.slice(i, i + sharedSpanPrefix.length) !== sharedSpanPrefix) throw new Error('expected empty shared_span');
	i += sharedSpanPrefix.length;
	expect(')\n');
	// Build TreeNode without propertyData (to be attached by outer decoder)
	return new TreeNode({
		labelRule: label_rule,
		description,
		propertyInfos: infos,
		version,
		childNames,
		propertyData: new Uint8Array(0),
		queryHowTo: qhFlag ? (qhFlag === 'J' ? Just(qh.value) : Nothing) : (qh.value ? Just(qh.value) : Nothing),
		qaSequence: qaFlag ? (qaFlag === 'J' ? Just(qa.value) : Nothing) : (qa.value ? Just(qa.value) : Nothing),
	});
}

// Pseudo code for encodeChunks_MaybeTreeNode
// 1. If mnode is nothing, encodeChunks_long_string("Nothing") and return
// 2. If mnode is just, text = a string "Just " followed by the TreeNode (via ostream operator, not including the property data)
// 3. Append 8 * 2 hex bytes to text for the number of property data chunks.
// 4. Start with encoded ChunkList = encodeChunks_long_string(text);
// 5. Append property data chunks to encoded
export function encodeChunks_MaybeTreeNode(requestId, signal, maybeNode) {
	if (!Maybe.isMaybe(maybeNode)) {
		// treat falsy as Nothing
		if (!maybeNode) return encodeChunks_long_string(requestId, signal, 'Nothing');
	}
	if (Maybe.isMaybe(maybeNode) && maybeNode.isNothing()) {
		return encodeChunks_long_string(requestId, signal, 'Nothing');
	}
	const node = Maybe.isMaybe(maybeNode) ? maybeNode.getOrElse(null) : maybeNode;
	const text = 'Just ' + formatTreeNodeText(node) + '\n';
	// Flatten property data to chunks with same signal/id
	const contentsChunks = flattenWithSignal(node.getPropertyData ? node.getPropertyData() : (node.propertyData || new Uint8Array(0)), signal, requestId);
	const hexCount = contentsChunks.length.toString(16).padStart(16, '0');
	const fullText = text + hexCount;
	const strChunks = encodeChunks_long_string(requestId, signal, fullText);
	return [...strChunks, ...contentsChunks];
}

export function canDecodeChunks_MaybeTreeNode(startChunk, chunks) {
	const afterStr = canDecodeChunks_long_string(startChunk, chunks);
	if (!afterStr) return null;
	// Peek last chunk of the string to see if trailing 16 hex chars exist; if not, it could be "Nothing"
	const strSeg = chunks.slice(startChunk, afterStr);
	const bytes = collectPayloadBytes(strSeg);
	const len = Number(le64Decode(bytes, 0));
	const textBytes = bytes.slice(8, 8 + len);
	const text = utf8Decode(textBytes);
	if (text === 'Nothing') return afterStr;
	// Otherwise, must have hex on the end
	// Safest is to accept and let decodeChunks_MaybeTreeNode compute next index using chunk count encoded; here we just ensure afterStr exists
	// and there are enough subsequent chunks as indicated by hex. If hex isn't present yet, return null.
	if (text.length < 16) return null;
	const hex = text.slice(-16);
	if (!/^[0-9a-fA-F]{16}$/.test(hex)) return null;
	const count = parseInt(hex, 16) >>> 0;
	const end = afterStr + count;
	if (end > chunks.length) return null;
	return end;
}

export function decodeChunks_MaybeTreeNode(startChunk, chunks) {
	const afterStr = canDecodeChunks_long_string(startChunk, chunks);
	if (!afterStr) throw new Error('cannot decode MaybeTreeNode');
	const strSeg = chunks.slice(startChunk, afterStr);
	const bytes = collectPayloadBytes(strSeg);
	const len = Number(le64Decode(bytes, 0));
	const text = utf8Decode(bytes.slice(8, 8 + len));
	if (text === 'Nothing') {
		return { nextChunk: afterStr, value: Nothing };
	}
	// Expect "Just " + node text + "\n" + 16 hex
	if (!text.startsWith('Just ')) throw new Error('bad MaybeTreeNode text');
	// Strip trailing 16-hex
	if (text.length < 16) throw new Error('missing hex');
	const hex = text.slice(-16);
	const count = parseInt(hex, 16) >>> 0;
	const nodeText = text.slice(5, text.length - 16); // remove 'Just ' prefix and hex suffix
	const node = parseTreeNodeText(nodeText);
	// Collect following property_data chunks
	const end = afterStr + count;
	if (end > chunks.length) throw new Error('insufficient chunks for property_data');
	const propBytes = collectPayloadBytes(chunks.slice(afterStr, end));
	// Attach
	node.setPropertyData(propBytes);
	return { nextChunk: end, value: Just(node) };
}

// ---- Chunk-based SequentialNotification (C++ parity) ----
// Encode a SequentialNotification as:
//   long_string("<signalCount> <label>") followed by Maybe<TreeNode> chunks
// Pseudo code for encodeChunks_SequentialNotification
// 1. Create a string text = notification sequence number + " " + node label notification is for;
// 2. ChunkList encoded = encodeChunks_long_string(text);
// 3. Append encodeChunks_MaybeTreeNode(the node as modified when originating the notification)
export function encodeChunks_SequentialNotification(requestId, signal, sn) {
	const seq = (sn?.signalCount >>> 0) || 0;
	const label = String(sn?.notification?.labelRule || '');
	const text = `${seq} ${label}`;
	const strChunks = encodeChunks_long_string(requestId, signal, text);
	const maybeChunks = encodeChunks_MaybeTreeNode(requestId, signal, sn?.notification?.maybeNode ?? Nothing);
	return [...strChunks, ...maybeChunks];
}

export function canDecodeChunks_SequentialNotification(startChunk, chunks) {
	const afterStr = canDecodeChunks_long_string(startChunk, chunks);
	if (!afterStr) return null;
	const maybeEnd = canDecodeChunks_MaybeTreeNode(afterStr, chunks);
	return maybeEnd || null;
}

export function decodeChunks_SequentialNotification(startChunk, chunks) {
	const { nextChunk: afterStr, value: text } = decodeChunks_long_string(startChunk, chunks);
	// text is "<signalCount> <label>"
	const sp = text.indexOf(' ');
	const signalCount = sp >= 0 ? parseInt(text.slice(0, sp), 10) >>> 0 : (parseInt(text, 10) >>> 0);
	const labelRule = sp >= 0 ? text.slice(sp + 1) : '';
	const { nextChunk, value: maybeNode } = decodeChunks_MaybeTreeNode(afterStr, chunks);
	return { nextChunk, value: { signalCount, notification: { labelRule, maybeNode } } };
}

// Vector<SequentialNotification> using chunk-based encoding (C++ parity)
// Count is encoded as a label (stringified number) in a single chunk
// Pseudo code for encodeChunks_VectorSequentialNotification
// 1. chunkList encoded = encodeChunks_label(notifications.size())
// 2. for each notification in notifications, append encodeChunks_SequentialNotification(notification) to encoded
export function encodeChunks_VectorSequentialNotification(requestId, signal, vec) {
	const countStr = String((vec?.length ?? 0) >>> 0);
	const out = encodeChunks_label(requestId, signal, countStr);
	for (const sn of (vec || [])) {
		out.push(...encodeChunks_SequentialNotification(requestId, signal, sn));
	}
	return out;
}

export function canDecodeChunks_VectorSequentialNotification(startChunk, chunks) {
	const afterCount = canDecodeChunks_label(startChunk, chunks);
	if (!afterCount) return null;
	const { value: countStr } = decodeChunks_label(startChunk, chunks);
	// Strictly require decimal digits for count; otherwise it's not a valid chunk-based vector
	if (!/^\d+$/.test(countStr)) return null;
	const n = parseInt(countStr, 10) >>> 0;
	let idx = afterCount;
	for (let i = 0; i < n; i++) {
		const nxt = canDecodeChunks_SequentialNotification(idx, chunks);
		if (!nxt) return null;
		idx = nxt;
	}
	return idx;
}

export function decodeChunks_VectorSequentialNotification(startChunk, chunks) {
	const afterCount = canDecodeChunks_label(startChunk, chunks);
	if (!afterCount) throw new Error('cannot decode vector count');
	const { value: countStr } = decodeChunks_label(startChunk, chunks);
	const n = parseInt(countStr, 10) >>> 0;
	const out = [];
	let idx = afterCount;
	for (let i = 0; i < n; i++) {
		const { nextChunk, value } = decodeChunks_SequentialNotification(idx, chunks);
		out.push(value);
		idx = nextChunk;
	}
	return { nextChunk: idx, value: out };
}


// Implementation of encodeChunks_NewNodeVersion
// newNodeVersion: [maybeVersion, [labelRule, maybeTreeNode]]
export function encodeChunks_NewNodeVersion(requestId, signal, newNodeVersion) {
	// newNodeVersion: [maybeVersion, [labelRule, maybeTreeNode]]
	const [maybeVersion, [labelRule, maybeTreeNode]] = newNodeVersion;
	let text;
	if (!maybeVersion || (typeof maybeVersion === 'object' && !('value' in maybeVersion) && !maybeVersion.isJust?.())) {
		// treat as Nothing
		text = 'Nothing';
	} else {
		// Just <version>
		let versionValue = (typeof maybeVersion === 'object' && 'value' in maybeVersion) ? maybeVersion.value : maybeVersion;
		text = `Just ${versionValue}`;
	}
	text += ` ${labelRule}`;
	// 1. encode long string
	let encoded = encodeChunks_long_string(requestId, signal, text);
	// 2. append encodeChunks_MaybeTreeNode
	let maybeTreeChunks = encodeChunks_MaybeTreeNode(requestId, signal, maybeTreeNode);
	encoded.push(...maybeTreeChunks);
	return encoded;
}

// Pseudo code for decodeChunks_NewNodeVersion
// 1. decode long_string for version and label
// 2. parse version and label from string
// 3. decode MaybeTreeNode from following chunks
export function decodeChunks_NewNodeVersion(startChunk, chunks) {
	const { nextChunk: afterStr, value: text } = decodeChunks_long_string(startChunk, chunks);
	// text is "Nothing <label>" or "Just <version> <label>"
	let version, label;
	if (text.startsWith('Nothing')) {
		version = Nothing;
		label = text.slice(8); // skip 'Nothing '
	} else if (text.startsWith('Just ')) {
		const sp = text.indexOf(' ', 5);
		version = sp > 0 ? Just(parseInt(text.slice(5, sp), 10)) : Nothing;
		label = sp > 0 ? text.slice(sp + 1) : '';
	} else {
		throw new Error('bad NewNodeVersion text');
	}
	const { nextChunk, value: maybeNode } = decodeChunks_MaybeTreeNode(afterStr, chunks);
	return { nextChunk, value: [version, [label, maybeNode]] };
}

// Pseudo code for canDecodeChunks_NewNodeVersion
// 1. can decode long_string?
// 2. can decode MaybeTreeNode after?
export function canDecodeChunks_NewNodeVersion(startChunk, chunks) {
	const afterStr = canDecodeChunks_long_string(startChunk, chunks);
	if (!afterStr) return null;
	const maybeEnd = canDecodeChunks_MaybeTreeNode(afterStr, chunks);
	return maybeEnd || null;
}

// Implementation of encodeChunks_SubTransaction
// subTransaction: [mainNewNodeVersion, descendantNewNodeVersions[]]
export function encodeChunks_SubTransaction(requestId, signal, subTransaction) {
	const [mainNewNodeVersion, descendantNewNodeVersions] = subTransaction;
	// 1. encode label for count of descendants
	let encoded = encodeChunks_label(requestId, signal, String(descendantNewNodeVersions.length));
	// 2. append main new node version
	let mainChunks = encodeChunks_NewNodeVersion(requestId, signal, mainNewNodeVersion);
	encoded.push(...mainChunks);
	// 3. append each descendant new node version
	for (const nnv of descendantNewNodeVersions) {
		let descChunks = encodeChunks_NewNodeVersion(requestId, signal, nnv);
		encoded.push(...descChunks);
	}
	return encoded;
}

// Pseudo code for decodeChunks_SubTransaction
// 1. decode label for descendant count
// 2. decode main NewNodeVersion
// 3. decode each descendant NewNodeVersion
export function decodeChunks_SubTransaction(startChunk, chunks) {
	const afterCount = canDecodeChunks_label(startChunk, chunks);
	if (!afterCount) throw new Error('cannot decode subtransaction count');
	const { value: countStr } = decodeChunks_label(startChunk, chunks);
	const n = parseInt(countStr, 10) >>> 0;
	let idx = afterCount;
	const { nextChunk: afterMain, value: main } = decodeChunks_NewNodeVersion(idx, chunks);
	idx = afterMain;
	const descendants = [];
	for (let i = 0; i < n; i++) {
		const { nextChunk, value } = decodeChunks_NewNodeVersion(idx, chunks);
		descendants.push(value);
		idx = nextChunk;
	}
	return { nextChunk: idx, value: [main, descendants] };
}

// Pseudo code for canDecodeChunks_SubTransaction
// 1. can decode label?
// 2. can decode main NewNodeVersion?
// 3. can decode each descendant NewNodeVersion?
export function canDecodeChunks_SubTransaction(startChunk, chunks) {
	const afterCount = canDecodeChunks_label(startChunk, chunks);
	if (!afterCount) return null;
	const { value: countStr } = decodeChunks_label(startChunk, chunks);
	if (!/^[0-9]+$/.test(countStr)) return null;
	const n = parseInt(countStr, 10) >>> 0;
	let idx = afterCount;
	const mainEnd = canDecodeChunks_NewNodeVersion(idx, chunks);
	if (!mainEnd) return null;
	idx = mainEnd;
	for (let i = 0; i < n; i++) {
		const nxt = canDecodeChunks_NewNodeVersion(idx, chunks);
		if (!nxt) return null;
		idx = nxt;
	}
	return idx;
}

// Implementation of encodeChunks_Transaction
// transaction: array of subTransactions
export function encodeChunks_Transaction(requestId, signal, transaction) {
	// 1. encode label for transaction size
	let encoded = encodeChunks_label(requestId, signal, String(transaction.length));
	// 2. append each subTransaction
	for (const subTx of transaction) {
		let subChunks = encodeChunks_SubTransaction(requestId, signal, subTx);
		encoded.push(...subChunks);
	}
	return encoded;
}

// Pseudo code for decodeChunks_Transaction
// 1. decode label for transaction size
// 2. decode each subTransaction
export function decodeChunks_Transaction(startChunk, chunks) {
	const afterCount = canDecodeChunks_label(startChunk, chunks);
	if (!afterCount) throw new Error('cannot decode transaction count');
	const { value: countStr } = decodeChunks_label(startChunk, chunks);
	const n = parseInt(countStr, 10) >>> 0;
	let idx = afterCount;
	const out = [];
	for (let i = 0; i < n; i++) {
		const { nextChunk, value } = decodeChunks_SubTransaction(idx, chunks);
		out.push(value);
		idx = nextChunk;
	}
	return { nextChunk: idx, value: out };
}

// Pseudo code for canDecodeChunks_Transaction
// 1. can decode label?
// 2. can decode each subTransaction?
export function canDecodeChunks_Transaction(startChunk, chunks) {
	const afterCount = canDecodeChunks_label(startChunk, chunks);
	if (!afterCount) return null;
	const { value: countStr } = decodeChunks_label(startChunk, chunks);
	if (!/^[0-9]+$/.test(countStr)) return null;
	const n = parseInt(countStr, 10) >>> 0;
	let idx = afterCount;
	for (let i = 0; i < n; i++) {
		const nxt = canDecodeChunks_SubTransaction(idx, chunks);
		if (!nxt) return null;
		idx = nxt;
	}
	return idx;
}

// Pseudo code for encodeChunks_VectorTreeNode
// 1. ChunkList encoded = encodeChunks_label(vector_size)
// 2. for each node in the vector, append encodeChunks_MaybeTreeNode(node) to encoded
export function encodeChunks_VectorTreeNode(requestId, signal, nodes) {
	const countStr = String(nodes.length >>> 0);
	const out = encodeChunks_label(requestId, signal, countStr);
	for (const n of nodes) {
		const m = encodeChunks_MaybeTreeNode(requestId, signal, Just(n));
		out.push(...m);
	}
	return out;
}

export function canDecodeChunks_VectorTreeNode(startChunk, chunks) {
	const afterCount = canDecodeChunks_label(startChunk, chunks);
	if (!afterCount) return null;
	const { value: countStr } = decodeChunks_label(startChunk, chunks);
	if (!/^\d+$/.test(countStr)) return null;
	const count = parseInt(countStr, 10) >>> 0;
	let idx = afterCount;
	for (let i = 0; i < count; i++) {
		const nxt = canDecodeChunks_MaybeTreeNode(idx, chunks);
		if (!nxt) return null;
		idx = nxt;
	}
	return idx;
}

export function decodeChunks_VectorTreeNode(startChunk, chunks) {
	const afterCount = canDecodeChunks_label(startChunk, chunks);
	if (!afterCount) throw new Error('cannot decode vector count');
	const { value: countStr } = decodeChunks_label(startChunk, chunks);
	const count = parseInt(countStr, 10) >>> 0;
	const out = [];
	let idx = afterCount;
	for (let i = 0; i < count; i++) {
		const { nextChunk, value } = decodeChunks_MaybeTreeNode(idx, chunks);
		if (value && value.isJust && value.isJust()) out.push(value.getOrElse(null));
		idx = nextChunk;
	}
	return { nextChunk: idx, value: out };
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
	throw new Error('Deprecated legacy helper encode_tree_node: use encodeChunks_MaybeTreeNode/Vector variants');
}
export function can_decode_tree_node(bytes, offset = 0) {
	throw new Error('Deprecated legacy helper can_decode_tree_node: use chunk-based decoders');
}
export function decode_tree_node(bytes, offset = 0) {
	throw new Error('Deprecated legacy helper decode_tree_node: use chunk-based decoders');
}

// Maybe<TreeNode> : uint8 flag(0/1) + [TreeNode]
export function encode_maybe_tree_node(maybeNode) {
	throw new Error('Deprecated legacy helper encode_maybe_tree_node: use encodeChunks_MaybeTreeNode');
}
export function can_decode_maybe_tree_node(bytes, offset = 0) {
	throw new Error('Deprecated legacy helper can_decode_maybe_tree_node: use canDecodeChunks_MaybeTreeNode');
}
export function decode_maybe_tree_node(bytes, offset = 0) {
	throw new Error('Deprecated legacy helper decode_maybe_tree_node: use decodeChunks_MaybeTreeNode');
}

// SequentialNotification: { signalCount:u32, notification: { labelRule, maybeNode } }
export function encode_sequential_notification(sn) {
	throw new Error('Deprecated legacy helper encode_sequential_notification: use encodeChunks_SequentialNotification');
}
export function can_decode_sequential_notification(bytes, offset = 0) {
	throw new Error('Deprecated legacy helper can_decode_sequential_notification: use canDecodeChunks_SequentialNotification');
}
export function decode_sequential_notification(bytes, offset = 0) {
	throw new Error('Deprecated legacy helper decode_sequential_notification: use decodeChunks_SequentialNotification');
}

// Vector<SequentialNotification>
export function encode_vec_sequential_notification(vec) {
	throw new Error('Deprecated legacy helper encode_vec_sequential_notification: use encodeChunks_VectorSequentialNotification');
}
export function can_decode_vec_sequential_notification(bytes, offset = 0) {
	throw new Error('Deprecated legacy helper can_decode_vec_sequential_notification: use canDecodeChunks_VectorSequentialNotification');
}
export function decode_vec_sequential_notification(bytes, offset = 0) {
	throw new Error('Deprecated legacy helper decode_vec_sequential_notification: use decodeChunks_VectorSequentialNotification');
}

// Vector<T> helpers
export function encode_vec(items, encItemFn) {
	throw new Error('Deprecated legacy helper encode_vec: use chunk-based vector encoders');
}
export function can_decode_vec(bytes, offset, canDecItemFn, peekSizeFn) {
	throw new Error('Deprecated legacy helper can_decode_vec: use chunk-based vector decoders');
}
export function decode_vec(bytes, offset, decItemFn) {
	throw new Error('Deprecated legacy helper decode_vec: use chunk-based vector decoders');
}

// NewNodeVersion = [Maybe<uint16>, [labelRule, Maybe<TreeNode>]]
export function encode_new_node_version(nnv) {
	throw new Error('Deprecated legacy helper encode_new_node_version: use encodeChunks_NewNodeVersion');
}
export function can_decode_new_node_version(bytes, offset = 0) {
	throw new Error('Deprecated legacy helper can_decode_new_node_version: use canDecodeChunks_NewNodeVersion');
}
export function decode_new_node_version(bytes, offset = 0) {
	throw new Error('Deprecated legacy helper decode_new_node_version: use decodeChunks_NewNodeVersion');
}

// SubTransaction = [NewNodeVersion, NewNodeVersion[]]
export function encode_sub_transaction(st) {
	throw new Error('Deprecated legacy helper encode_sub_transaction: use encodeChunks_SubTransaction');
}
export function can_decode_sub_transaction(bytes, offset = 0) {
	throw new Error('Deprecated legacy helper can_decode_sub_transaction: use canDecodeChunks_SubTransaction');
}
export function decode_sub_transaction(bytes, offset = 0) {
	throw new Error('Deprecated legacy helper decode_sub_transaction: use decodeChunks_SubTransaction');
}

// Transaction = SubTransaction[]
// Replace legacy Transaction helpers with throwing stubs
export function encode_transaction(tx) { throw new Error('Deprecated legacy helper encode_transaction: use encodeChunks_Transaction'); }
export function can_decode_transaction(bytes, offset = 0) { throw new Error('Deprecated legacy helper can_decode_transaction: use canDecodeChunks_Transaction'); }
export function decode_transaction(bytes, offset = 0) { throw new Error('Deprecated legacy helper decode_transaction: use decodeChunks_Transaction'); }

// Vector<TreeNode>
// Replace legacy Vector<TreeNode> helpers with throwing stubs
export function encode_vec_tree_node(nodes) { throw new Error('Deprecated legacy helper encode_vec_tree_node: use encodeChunks_VectorTreeNode'); }
export function can_decode_vec_tree_node(bytes, offset = 0) { throw new Error('Deprecated legacy helper can_decode_vec_tree_node: use canDecodeChunks_VectorTreeNode'); }
export function decode_vec_tree_node(bytes, offset = 0) { throw new Error('Deprecated legacy helper decode_vec_tree_node: use decodeChunks_VectorTreeNode'); }

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

