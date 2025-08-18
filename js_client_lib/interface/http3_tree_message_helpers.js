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
	const qhs = (Maybe.isMaybe(qh) && qh.isJust()) ? String(qh.value) : '';
	out += `query_how_to ${qhs.length} :\n${qhs}\n`;
	// qa_sequence
	const qa = tn.getQaSequence();
	const qas = (Maybe.isMaybe(qa) && qa.isJust()) ? String(qa.value) : '';
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
	const label_rule = readUntilNewline('label_rule: ');
	const d = parseLengthString(text, i, 'description'); i = d.next; const description = d.value;
	// property_infos
	expect('property_infos: [ ');
	const propLine = text.slice(i, text.indexOf('], ', i));
	const infos = [];
	if (propLine && propLine.length > 0) {
		const items = propLine.split(',').map(s => s.trim()).filter(Boolean);
		for (const it of items) {
			// format: TYPE (NAME)
			const m = it.match(/^(\S+) \(([^)]+)\)$/);
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
		queryHowTo: qh.value ? Just(qh.value) : Nothing,
		qaSequence: qa.value ? Just(qa.value) : Nothing,
	});
}

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

