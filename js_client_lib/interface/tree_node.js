// TreeNode & related types – browser-safe implementation
// Parity target: C++ tree_node.h with JS-idiomatic APIs.
// Uses Uint8Array/DataView, no Node Buffer.

import { Maybe, Just, Nothing, fromNullable } from './maybe.js';

// ---- TreeNodeVersion ----
export class TreeNodeVersion {
	constructor({
		versionNumber = 0,
		maxVersionSequence = 256,
		policy = 'default',
		authorialProof = Nothing,
		authors = Nothing,
		readers = Nothing,
		collisionDepth = Nothing,
	} = {}) {
		this.versionNumber = toUint(versionNumber, 16) ?? 0;
		this.maxVersionSequence = toUint(maxVersionSequence, 16) ?? 256;
		this.policy = policy;
		this.authorialProof = Maybe.isMaybe(authorialProof) ? authorialProof : fromNullable(authorialProof);
		this.authors = Maybe.isMaybe(authors) ? authors : fromNullable(authors);
		this.readers = Maybe.isMaybe(readers) ? readers : fromNullable(readers);
		this.collisionDepth = Maybe.isMaybe(collisionDepth) ? collisionDepth : fromNullable(collisionDepth);
	}

	clone() {
		return new TreeNodeVersion({
			versionNumber: this.versionNumber,
			maxVersionSequence: this.maxVersionSequence,
			policy: this.policy,
			authorialProof: this.authorialProof,
			authors: this.authors,
			readers: this.readers,
			collisionDepth: this.collisionDepth,
		});
	}

	increment() {
		const max = this.maxVersionSequence >>> 0;
		if (!max || max <= 0) return this;
		this.versionNumber = ((this.versionNumber >>> 0) + 1) % max;
		return this;
	}

	isDefault() {
		return (
			this.versionNumber === 0 &&
			this.maxVersionSequence === 256 &&
			this.policy === 'default' &&
			this.authorialProof.isNothing() &&
			this.authors.isNothing() &&
			this.readers.isNothing() &&
			this.collisionDepth.isNothing()
		);
	}

	// Wrap-around comparisons using half-range rule.
	// eq: numeric equality
	// lt: b - a in (0, max/2]
	// gt: a - b in (0, max/2]
	// le/ge include eq.
	eq(other) {
		return this.versionNumber === other.versionNumber;
	}

	lt(other) {
		const max = this._cmpMax(other);
		if (this.eq(other)) return false;
		const delta = ((other.versionNumber >>> 0) - (this.versionNumber >>> 0) + max) % max;
		return delta > 0 && delta <= max / 2;
	}

	le(other) {
		return this.eq(other) || this.lt(other);
	}

	gt(other) {
		const max = this._cmpMax(other);
		if (this.eq(other)) return false;
		const delta = ((this.versionNumber >>> 0) - (other.versionNumber >>> 0) + max) % max;
		return delta > 0 && delta <= max / 2;
	}

	ge(other) {
		return this.eq(other) || this.gt(other);
	}

	_cmpMax(other) {
		// Prefer this.maxVersionSequence; if they differ, use Math.min to retain half-range logic.
		const a = this.maxVersionSequence >>> 0;
		const b = (other?.maxVersionSequence ?? a) >>> 0;
		return Math.max(1, Math.min(a || 256, b || 256));
	}

	toJSON() {
		return {
			versionNumber: this.versionNumber,
			maxVersionSequence: this.maxVersionSequence,
			policy: this.policy,
			authorialProof: this.authorialProof.toJSON(),
			authors: this.authors.toJSON(),
			readers: this.readers.toJSON(),
			collisionDepth: this.collisionDepth.toJSON(),
		};
	}

	static fromJSON(obj) {
		if (!obj || typeof obj !== 'object') throw new TypeError('TreeNodeVersion.fromJSON expects object');
		return new TreeNodeVersion({
			versionNumber: toUint(obj.versionNumber, 16) ?? 0,
			maxVersionSequence: toUint(obj.maxVersionSequence, 16) ?? 256,
			policy: validatePolicy(obj.policy ?? 'default'),
			authorialProof: maybeFromJSON(obj.authorialProof),
			authors: maybeFromJSON(obj.authors),
			readers: maybeFromJSON(obj.readers),
			collisionDepth: maybeFromJSON(obj.collisionDepth),
		});
	}
}

function validatePolicy(policy) {
	if (typeof policy !== 'string' || policy.length === 0) throw new Error('TreeNodeVersion.policy must be non-empty string');
	return policy;
}

function maybeFromJSON(x) {
	if (x && typeof x === 'object' && (x.kind === 'just' || x.kind === 'nothing')) {
		return x.kind === 'nothing' ? Nothing : Just(x.value);
	}
	return fromNullable(x);
}

function toUint(value, bits) {
	if (value === undefined || value === null) return undefined;
	const v = Number(value);
	if (!Number.isFinite(v) || v < 0) return 0;
	if (bits === 16) return v & 0xffff;
	if (bits === 32) return v >>> 0;
	return v >>> 0;
}

// ---- TreeNode ----
export class TreeNode {
	constructor({
		labelRule = '',
		description = '',
		propertyInfos = [], // [{type, name}]
		version = new TreeNodeVersion(),
		childNames = [],
		propertyData = new Uint8Array(0),
		queryHowTo = Nothing,
		qaSequence = Nothing,
	} = {}) {
		this.labelRule = String(labelRule);
		this.description = String(description);
		this.propertyInfos = propertyInfos.map(({ type, name }) => ({ type: String(type), name: String(name) }));
		this.version = version instanceof TreeNodeVersion ? version : new TreeNodeVersion(version || {});
		this.childNames = [...childNames].map(String);
		this.propertyData = propertyData instanceof Uint8Array ? propertyData : new Uint8Array(propertyData || 0);
		this.queryHowTo = Maybe.isMaybe(queryHowTo) ? queryHowTo : fromNullable(queryHowTo);
		this.qaSequence = Maybe.isMaybe(qaSequence) ? qaSequence : fromNullable(qaSequence);
	}

	clone() {
		return new TreeNode({
			labelRule: this.labelRule,
			description: this.description,
			propertyInfos: this.propertyInfos.map((p) => ({ ...p })),
			version: this.version.clone(),
			childNames: [...this.childNames],
			propertyData: this.propertyData.slice(),
			queryHowTo: this.queryHowTo,
			qaSequence: this.qaSequence,
		});
	}

	// Getters/Setters
	getLabelRule() { return this.labelRule; }
	setLabelRule(v) { this.labelRule = String(v); }

	getDescription() { return this.description; }
	setDescription(v) { this.description = String(v); }

	getPropertyInfos() { return this.propertyInfos.map((p) => ({ ...p })); }
	setPropertyInfos(arr) { this.propertyInfos = arr.map(({ type, name }) => ({ type: String(type), name: String(name) })); }

	getVersion() { return this.version; }
	setVersion(v) { this.version = v instanceof TreeNodeVersion ? v : new TreeNodeVersion(v || {}); }

	getChildNames() { return [...this.childNames]; }
	setChildNames(arr) { this.childNames = arr.map(String); }

	getPropertyData() { return this.propertyData; }
	setPropertyData(u8) { this.propertyData = u8 instanceof Uint8Array ? u8 : new Uint8Array(u8 || 0); }

	getQueryHowTo() { return this.queryHowTo; }
	setQueryHowTo(v) { this.queryHowTo = Maybe.isMaybe(v) ? v : fromNullable(v); }

	getQaSequence() { return this.qaSequence; }
	setQaSequence(v) { this.qaSequence = Maybe.isMaybe(v) ? v : fromNullable(v); }

	// Path helpers
	getNodeName() {
		const idx = this.labelRule.lastIndexOf('/');
		return idx === -1 ? this.labelRule : this.labelRule.slice(idx + 1);
	}

	getNodePath() {
		const idx = this.labelRule.lastIndexOf('/');
		return idx === -1 ? '' : this.labelRule.slice(0, idx);
	}

	getAbsoluteChildNames() {
		const base = this.getNodePath();
		if (!base) return this.childNames.map(String);
		return this.childNames.map((c) => (c ? `${base}/${c}` : base));
	}

	// Version bump equivalent
	bumpVersion() { this.version.increment(); return this; }

	// Equality check (deep)
	equals(other) {
		if (!other) return false;
		if (this.labelRule !== other.labelRule) return false;
		if (this.description !== other.description) return false;
		if (!versionEquals(this.version, other.version)) return false;
		if (!arrayEq(this.childNames, other.childNames)) return false;
		if (!propInfosEq(this.propertyInfos, other.propertyInfos)) return false;
		if (!u8Eq(this.propertyData, other.propertyData)) return false;
		if (!this.queryHowTo.equals(other.queryHowTo)) return false;
		if (!this.qaSequence.equals(other.qaSequence)) return false;
		return true;
	}

	// Label prefix helpers
	prefixLabels(prefix) {
		if (!prefix) return this;
		const pfx = String(prefix);
		this.labelRule = `${pfx}/${this.labelRule}`;
		this.childNames = this.childNames.map((c) => `${pfx}/${c}`);
		return this;
	}

	shortenLabels(prefix) {
		if (!prefix) return this;
		const pfx = String(prefix) + '/';
		const strip = (s) => (s.startsWith(pfx) ? s.slice(pfx.length) : s);
		this.labelRule = strip(this.labelRule);
		this.childNames = this.childNames.map(strip);
		return this;
	}

	// ---- Property data helpers ----
	layoutOffsets() {
		return computeLayout(this.propertyInfos, this.propertyData);
	}

	getPropertyValue(name) {
		const { map } = this.layoutOffsets();
		const meta = map.get(name);
		if (!meta) throw new Error(`Property not found: ${name}`);
		const { type, offset, span, variable } = meta;
		const dv = new DataView(this.propertyData.buffer, this.propertyData.byteOffset, this.propertyData.byteLength);
		if (!variable) {
			const value = readFixed(type, dv, offset);
			const raw = this.propertyData.slice(offset, offset + span);
			return [span, value, raw];
		} else {
			const data = this.propertyData.slice(offset + 4, offset + span);
			// Generic value for variable is Uint8Array; use getPropertyString for strings
			return [span, data, data];
		}
	}

	getPropertyString(name) {
		const { map } = this.layoutOffsets();
		const meta = map.get(name);
		if (!meta) throw new Error(`Property not found: ${name}`);
		const { offset, span, variable } = meta;
		if (!variable) throw new Error('Property is fixed-size; expected variable-size string');
		const data = this.propertyData.slice(offset + 4, offset + span);
		return [span, utf8Decode(data)];
	}

	getPropertyValueSpan(name) {
		const { map } = this.layoutOffsets();
		const meta = map.get(name);
		if (!meta) throw new Error(`Property not found: ${name}`);
		const { offset, span, variable } = meta;
		if (!variable) throw new Error('Property is fixed-size; expected variable-size span');
		const _header = this.propertyData.slice(offset, offset + 4);
		const data = this.propertyData.slice(offset + 4, offset + span);
		return [span, _header, data];
	}

	setPropertyValue(name, value, typeHint) {
		const { map } = this.layoutOffsets();
		const meta = map.get(name);
		if (!meta) throw new Error(`Property not found: ${name}`);
		if (meta.variable) throw new Error('setPropertyValue expects fixed-size property');
		const type = meta.type || typeHint;
		if (!fixedSizeTypes.has(type)) throw new Error(`Unsupported fixed type: ${type}`);
		// Rebuild
		const parts = buildPropertiesWithChange(this.propertyInfos, this.propertyData, {
			action: 'set-fixed', name, type, value,
		});
		this.propertyData = parts;
		return this;
	}

	setPropertyString(name, str) {
		const data = utf8Encode(str);
		return this.setPropertyValueSpan(name, data);
	}

	setPropertyValueSpan(name, data) {
		const { map } = this.layoutOffsets();
		const meta = map.get(name);
		if (!meta) throw new Error(`Property not found: ${name}`);
		if (!meta.variable) throw new Error('setPropertyValueSpan expects variable-size property');
		const parts = buildPropertiesWithChange(this.propertyInfos, this.propertyData, {
			action: 'set-var', name, data,
		});
		this.propertyData = parts;
		return this;
	}

	insertProperty(index, name, value, type) {
		if (this.propertyInfos.find((p) => p.name === name)) throw new Error(`Property exists: ${name}`);
		let t = type;
		if (!t) t = inferFixedType(value);
		if (!fixedSizeTypes.has(t)) throw new Error('insertProperty for fixed types requires a supported type');
		const safeIndex = clampIndex(index, this.propertyInfos.length);
		const parts = buildPropertiesWithChange(this.propertyInfos, this.propertyData, {
			action: 'insert-fixed', index: safeIndex, name, type: t, value,
		});
		this.propertyInfos = insertAt(this.propertyInfos, safeIndex, { type: t, name });
		this.propertyData = parts;
		return this;
	}

	insertPropertyString(index, name, type, str) {
		if (this.propertyInfos.find((p) => p.name === name)) throw new Error(`Property exists: ${name}`);
		const data = utf8Encode(str);
		const safeIndex = clampIndex(index, this.propertyInfos.length);
		const parts = buildPropertiesWithChange(this.propertyInfos, this.propertyData, {
			action: 'insert-var', index: safeIndex, name, type, data,
		});
		this.propertyInfos = insertAt(this.propertyInfos, safeIndex, { type, name });
		this.propertyData = parts;
		return this;
	}

	insertPropertySpan(index, name, type, data) {
		if (this.propertyInfos.find((p) => p.name === name)) throw new Error(`Property exists: ${name}`);
		const safeIndex = clampIndex(index, this.propertyInfos.length);
		const parts = buildPropertiesWithChange(this.propertyInfos, this.propertyData, {
			action: 'insert-var', index: safeIndex, name, type, data,
		});
		this.propertyInfos = insertAt(this.propertyInfos, safeIndex, { type, name });
		this.propertyData = parts;
		return this;
	}

	deleteProperty(name) {
		const idx = this.propertyInfos.findIndex((p) => p.name === name);
		if (idx < 0) throw new Error(`Property not found: ${name}`);
		const parts = buildPropertiesWithChange(this.propertyInfos, this.propertyData, {
			action: 'delete', name,
		});
		this.propertyInfos = this.propertyInfos.filter((p) => p.name !== name);
		this.propertyData = parts;
		return this;
	}
}

function versionEquals(a, b) {
	return (
		a.versionNumber === b.versionNumber &&
		a.maxVersionSequence === b.maxVersionSequence &&
		a.policy === b.policy &&
		a.authorialProof.equals(b.authorialProof) &&
		a.authors.equals(b.authors) &&
		a.readers.equals(b.readers) &&
		a.collisionDepth.equals(b.collisionDepth)
	);
}

function arrayEq(a, b) {
	if (a.length !== b.length) return false;
	for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
	return true;
}

function propInfosEq(a, b) {
	if (a.length !== b.length) return false;
	for (let i = 0; i < a.length; i++) {
		if (a[i].type !== b[i].type || a[i].name !== b[i].name) return false;
	}
	return true;
}

function u8Eq(a, b) {
	if (!a && !b) return true;
	if (!a || !b) return false;
	if (a.byteLength !== b.byteLength) return false;
	for (let i = 0; i < a.byteLength; i++) if (a[i] !== b[i]) return false;
	return true;
}

export const fixedSizeTypes = new Set(['int64', 'uint64', 'double', 'float', 'bool']);

function sizeofType(type) {
	switch (type) {
		case 'int64':
		case 'uint64':
		case 'double':
			return 8;
		case 'float':
			return 4;
		case 'bool':
			return 1;
		default:
			return null; // variable-size
	}
}

function computeLayout(propertyInfos, propertyData) {
	const map = new Map();
	let offset = 0;
	const dv = new DataView(propertyData.buffer, propertyData.byteOffset, propertyData.byteLength);
	for (const { type, name } of propertyInfos) {
		const sz = sizeofType(type);
		if (sz) {
			map.set(name, { type, offset, span: sz, variable: false });
			offset += sz;
		} else {
			// var-size: 4-byte le length + bytes
			if (offset + 4 > propertyData.byteLength) throw new Error('Property data truncated');
			const len = dv.getUint32(offset, true);
			const span = 4 + len;
			if (offset + span > propertyData.byteLength) throw new Error('Property data truncated');
			map.set(name, { type, offset, span, variable: true });
			offset += span;
		}
	}
	if (offset !== propertyData.byteLength) {
		// Extra bytes at the end are not expected; allow but warn in dev.
		// noop
	}
	return { map, total: offset };
}

function readFixed(type, dv, offset) {
	switch (type) {
		case 'int64':
			ensureBigIntSupport();
			return dv.getBigInt64(offset, true);
		case 'uint64':
			ensureBigIntSupport();
			return dv.getBigUint64(offset, true);
		case 'double':
			return dv.getFloat64(offset, true);
		case 'float':
			return dv.getFloat32(offset, true);
		case 'bool':
			return dv.getUint8(offset) !== 0;
		default:
			throw new Error(`Unsupported fixed type: ${type}`);
	}
}

function writeFixed(type, value, out, offset) {
	const dv = new DataView(out.buffer, out.byteOffset, out.byteLength);
	switch (type) {
		case 'int64':
			ensureBigIntSupport();
			dv.setBigInt64(offset, BigInt(value), true);
			return 8;
		case 'uint64':
			ensureBigIntSupport();
			dv.setBigUint64(offset, BigInt(value), true);
			return 8;
		case 'double':
			dv.setFloat64(offset, Number(value), true);
			return 8;
		case 'float':
			dv.setFloat32(offset, Number(value), true);
			return 4;
		case 'bool':
			dv.setUint8(offset, value ? 1 : 0);
			return 1;
		default:
			throw new Error(`Unsupported fixed type: ${type}`);
	}
}

function ensureBigIntSupport() {
	if (typeof BigInt === 'undefined') throw new Error('BigInt not supported in this environment');
}

function utf8Encode(str) {
	return new TextEncoder().encode(String(str));
}

function utf8Decode(u8) {
	return new TextDecoder('utf-8', { fatal: false }).decode(u8);
}

function clampIndex(index, len) {
	const i = Number(index);
	if (!Number.isFinite(i) || i < 0) return len; // append on out-of-range
	return Math.min(i, len);
}

function insertAt(arr, index, item) {
	const out = arr.slice();
	out.splice(index, 0, item);
	return out;
}

function buildPropertiesWithChange(propertyInfos, propertyData, change) {
	// First compute existing values/spans to reconstruct efficiently.
	const { map } = computeLayout(propertyInfos, propertyData);
	// Precompute new total length
	let newLen = 0;
	const items = [];
	for (let i = 0; i < propertyInfos.length; i++) {
		const { type, name } = propertyInfos[i];
		const meta = map.get(name);
		let mode = 'copy';
		let span = meta?.span ?? 0;
		let fixed = !!sizeofType(type);
		if (change.action === 'set-fixed' && change.name === name) {
			mode = 'write-fixed';
			span = sizeofType(type);
			fixed = true;
		} else if (change.action === 'set-var' && change.name === name) {
			mode = 'write-var';
			span = 4 + (change.data?.byteLength ?? 0);
			fixed = false;
		} else if (change.action === 'delete' && change.name === name) {
			mode = 'skip';
			span = 0;
		}
		items.push({ index: i, type, name, mode, span, fixed, meta });
		newLen += span;
	}
	if (change.action === 'insert-fixed' || change.action === 'insert-var') {
		const span = change.action === 'insert-fixed' ? sizeofType(change.type) : 4 + (change.data?.byteLength ?? 0);
		newLen += span;
	}

	const out = new Uint8Array(newLen);
	let off = 0;
	const dvOut = new DataView(out.buffer, out.byteOffset, out.byteLength);

	for (let i = 0; i < items.length; i++) {
		const it = items[i];
		if (change.action === 'insert-fixed' && change.index === i) {
			// Write inserted fixed before existing i
			off += writeFixed(change.type, change.value, out, off);
		} else if (change.action === 'insert-var' && change.index === i) {
			dvOut.setUint32(off, change.data.byteLength, true); off += 4;
			out.set(change.data, off); off += change.data.byteLength;
		}

		if (it.mode === 'skip') continue;
		if (it.mode === 'copy') {
			// copy existing span
			const srcStart = it.meta.offset;
			const srcEnd = it.meta.offset + it.meta.span;
			out.set(propertyData.slice(srcStart, srcEnd), off); // copy
			off += it.meta.span;
		} else if (it.mode === 'write-fixed') {
			off += writeFixed(it.type, change.value, out, off);
		} else if (it.mode === 'write-var') {
			dvOut.setUint32(off, change.data.byteLength, true); off += 4;
			out.set(change.data, off); off += change.data.byteLength;
		}
	}

	if (change.action === 'insert-fixed' && change.index >= items.length) {
		off += writeFixed(change.type, change.value, out, off);
	} else if (change.action === 'insert-var' && change.index >= items.length) {
		dvOut.setUint32(off, change.data.byteLength, true); off += 4;
		out.set(change.data, off); off += change.data.byteLength;
	}

	return out;
}

function inferFixedType(value) {
	const t = typeof value;
	if (t === 'boolean') return 'bool';
	if (t === 'bigint') return 'int64'; // default to signed
	if (t === 'number') return 'double'; // default to double precision
	throw new Error('Cannot infer fixed type from value; pass explicit type');
}

// ---- YAML stubs ----
export function fromYAMLNode(_yamlLike) {
	// Placeholder for parity; implement later when YAML dependency is decided.
	throw new Error('fromYAMLNode not implemented');
}

export function toYAML(_treeNode) {
	// Placeholder for parity; implement later when YAML dependency is decided.
	throw new Error('toYAML not implemented');
}

// ---- Transaction types and helpers ----
// Shapes (documentation):
// NewNodeVersion = [Maybe<number /*uint16*/>, [string /*labelRule*/, Maybe<TreeNode>]]
// SubTransaction = [NewNodeVersion, NewNodeVersion[]]
// Transaction = SubTransaction[]

export function prefixNewNodeVersionLabels(prefix, nnv) {
	const [maybeVer, [label, maybeNode]] = nnv;
	const newLabel = prefix ? `${prefix}/${label}` : label;
	const newNode = Maybe.isMaybe(maybeNode) && maybeNode.isJust() ? Just(maybeNode.value.clone().prefixLabels(prefix)) : maybeNode;
	return [maybeVer, [newLabel, newNode]];
}

export function shortenNewNodeVersionLabels(prefix, nnv) {
	const [maybeVer, [label, maybeNode]] = nnv;
	const pfx = prefix ? String(prefix) + '/' : '';
	const newLabel = pfx && label.startsWith(pfx) ? label.slice(pfx.length) : label;
	const newNode = Maybe.isMaybe(maybeNode) && maybeNode.isJust() ? Just(maybeNode.value.clone().shortenLabels(prefix)) : maybeNode;
	return [maybeVer, [newLabel, newNode]];
}

export function prefixSubTransactionLabels(prefix, st) {
	const [parent, children] = st;
	return [prefixNewNodeVersionLabels(prefix, parent), children.map((c) => prefixNewNodeVersionLabels(prefix, c))];
}

export function shortenSubTransactionLabels(prefix, st) {
	const [parent, children] = st;
	return [shortenNewNodeVersionLabels(prefix, parent), children.map((c) => shortenNewNodeVersionLabels(prefix, c))];
}

export function prefixTransactionLabels(prefix, tx) {
	return tx.map((st) => prefixSubTransactionLabels(prefix, st));
}

export function shortenTransactionLabels(prefix, tx) {
	return tx.map((st) => shortenSubTransactionLabels(prefix, st));
}

export { Maybe, Just, Nothing };

