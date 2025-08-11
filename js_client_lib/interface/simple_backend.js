// SimpleBackend: in-memory implementation of Backend for browser-safe local caching and offline ops.
// Parity target: C++ memory/include/simple_backend.h (minimal semantics).
// Notes:
// - Data model: Map<string, TreeNode> keyed by labelRule
// - Query label rules: simple segment-wise wildcard '*' and prefix support via trailing '/*'.
// - Page tree: expect a variable-size property named 'page_nodes' containing a JSON array of label rules (UTF-8 string).
// - Transactions: apply atomically in-memory; NewNodeVersion shape from tree_node.js.

import { Backend } from './backend.js';
import { Maybe, Just, Nothing } from './maybe.js';
import { TreeNode } from './tree_node.js';

export default class SimpleBackend extends Backend {
  constructor() {
    super();
    this.nodes = new Map(); // labelRule -> TreeNode
    this.listeners = new Map(); // key -> { listenerName, labelRule, childNotify, cb }
  }

  // ---- CRUD ----
  getNode(labelRule) {
    const n = this.nodes.get(String(labelRule));
    return n ? Just(n.clone()) : Nothing;
  }

  upsertNode(nodes) {
    if (!Array.isArray(nodes)) throw new TypeError('upsertNode expects an array of TreeNode');
    const changedLabels = [];
    for (const node of nodes) {
      const n = node instanceof TreeNode ? node : new TreeNode(node || {});
      const label = n.getLabelRule();
      // store a clone to isolate external mutations
      this.nodes.set(label, n.clone());
      changedLabels.push(label);
    }
    // notify after all updates
    for (const label of changedLabels) {
      const maybeNode = this.getNode(label); // clone
      this.notifyListeners(label, maybeNode);
    }
    return true;
  }

  deleteNode(labelRule) {
    const label = String(labelRule);
    const existed = this.nodes.delete(label);
    if (existed) this.notifyListeners(label, Nothing);
    return existed;
  }

  // ---- Queries ----
  queryNodes(labelRule) {
    const rule = String(labelRule);
    const out = [];
    for (const n of this.nodes.values()) {
      if (matchRule(rule, n.getLabelRule())) out.push(n.clone());
    }
    return out;
  }

  relativeQueryNodes(node, labelRule) {
    const baseLabel = node instanceof TreeNode ? node.getLabelRule() : String(node?.getLabelRule ?? '');
    const effectiveRule = baseLabel ? joinPath(baseLabel, String(labelRule)) : String(labelRule);
    return this.queryNodes(effectiveRule);
  }

  // ---- Page tree ----
  getPageTree(pageNodeLabelRule) {
    const m = this.getNode(pageNodeLabelRule);
    if (m.isNothing()) return [];
    const pageNode = m.getOrElse(null);
    const list = readPageNodesList(pageNode);
    const out = [];
    for (const lr of list) {
      const n = this.nodes.get(lr);
      if (n) out.push(n.clone());
    }
    return out;
  }

  relativeGetPageTree(node, pageNodeLabelRule) {
    const baseLabel = node instanceof TreeNode ? node.getLabelRule() : String(node?.getLabelRule ?? '');
    const effective = baseLabel ? joinPath(baseLabel, String(pageNodeLabelRule)) : String(pageNodeLabelRule);
    return this.getPageTree(effective);
  }

  // ---- Transactions ----
  openTransactionLayer(_node) { throw new Error('UnsupportedOperation: SimpleBackend has no transaction layers'); }
  closeTransactionLayers() { throw new Error('UnsupportedOperation: SimpleBackend has no transaction layers'); }

  applyTransaction(tx) {
    // Transaction = Array<SubTransaction>
    // SubTransaction = [parentNNV, childNNVs[]]
    // NewNodeVersion = [Maybe<uint16>, [labelRule, Maybe<TreeNode>]]
    if (!Array.isArray(tx)) throw new TypeError('applyTransaction expects Transaction (array)');

    // Stage in a new map
    const staged = new Map(this.nodes);
    const affected = new Set();

    // Validate pass (lightweight)
    for (const st of tx) {
      if (!Array.isArray(st) || st.length !== 2) throw new Error('SubTransaction shape invalid');
      const [parent, children] = st;
      validateNNV(parent);
      if (!Array.isArray(children)) throw new Error('SubTransaction children must be array');
      for (const c of children) validateNNV(c);
    }

    // Apply to staged
    const applyNNV = (nnv) => {
      const [maybeVer, [labelRule, maybeNode]] = nnv;
      const label = String(labelRule);
      if (Maybe.isMaybe(maybeNode) && maybeNode.isNothing()) {
        staged.delete(label);
        affected.add(label);
        return;
      }
      const nodeVal = Maybe.isMaybe(maybeNode) && maybeNode.isJust() ? maybeNode.getOrElse(null) : null;
      let node = nodeVal ? (nodeVal instanceof TreeNode ? nodeVal.clone() : new TreeNode(nodeVal || {})) : staged.get(label)?.clone();
      if (!node) {
        // If no node available to update and no new node provided, create a bare node with matching label.
        node = new TreeNode({ labelRule: label });
      }
      // Ensure label alignment
      node.setLabelRule(label);
      if (Maybe.isMaybe(maybeVer) && maybeVer.isJust()) {
        const vnum = Number(maybeVer.getOrElse(0)) & 0xffff;
        const v = node.getVersion();
        v.versionNumber = vnum;
        node.setVersion(v);
      }
      staged.set(label, node);
      affected.add(label);
    };

    for (const st of tx) {
      const [parent, children] = st;
      applyNNV(parent);
      for (const c of children) applyNNV(c);
    }

    // Commit
    this.nodes = staged;

    // Notify listeners
    for (const label of affected) {
      const maybeNode = this.getNode(label);
      this.notifyListeners(label, maybeNode);
    }

    return true;
  }

  getFullTree() {
    return Array.from(this.nodes.values()).map((n) => n.clone());
  }

  // ---- Listeners ----
  registerNodeListener(listenerName, labelRule, childNotify, cb) {
    if (typeof cb !== 'function') throw new TypeError('listener callback must be a function');
    const key = listenerKey(listenerName, labelRule);
    this.listeners.set(key, { listenerName: String(listenerName), labelRule: String(labelRule), childNotify: !!childNotify, cb });
  }

  deregisterNodeListener(listenerName, labelRule) {
    const key = listenerKey(listenerName, labelRule);
    this.listeners.delete(key);
  }

  notifyListeners(labelRule, maybeNode) {
    const label = String(labelRule);
    for (const { labelRule: lr, childNotify, cb } of this.listeners.values()) {
      if (label === lr || (childNotify && rulesOverlap(lr, label))) {
        try { cb(this, label, maybeNode); } catch (_) { /* swallow listener errors */ }
      }
    }
  }

  processNotifications() {
    // No-op for SimpleBackend; notifications are synchronous
  }
}

// ---- Helpers ----
function listenerKey(name, rule) { return `${String(name)}|${String(rule)}`; }

function joinPath(base, rel) {
  if (!base) return rel;
  if (!rel) return base;
  if (rel.startsWith('/')) return rel; // already absolute-ish
  if (base.endsWith('/')) return base + rel;
  return `${base}/${rel}`;
}

// Match a label against a rule with basic wildcards.
// Supported:
// - '*' matches a single path segment
// - trailing '/*' means prefix match for any deeper descendant
// - exact segment equality otherwise
function matchRule(rule, label) {
  const r = String(rule);
  const l = String(label);
  if (r === l) return true;

  const rSegs = r.split('/').filter(Boolean);
  const lSegs = l.split('/').filter(Boolean);
  if (rSegs.length !== lSegs.length) return false;
  for (let i = 0; i < rSegs.length; i++) {
    const a = rSegs[i], b = lSegs[i];
    if (a === '*') continue;
    if (a !== b) return false;
  }
  return true;
}

// Overlap if one matches the other as a rule/label or they share an ancestor/descendant relationship.
function rulesOverlap(a, b) {
  if (a === b) return true;
  if (matchRule(a, b) || matchRule(b, a)) return true;
  // ancestor check
  return b.startsWith(a + '/') || a.startsWith(b + '/');
}

function validateNNV(nnv) {
  if (!Array.isArray(nnv) || nnv.length !== 2 || !Array.isArray(nnv[1]) || nnv[1].length !== 2) {
    throw new Error('NewNodeVersion shape invalid');
  }
  const [maybeVer, [labelRule, maybeNode]] = nnv;
  if (typeof labelRule !== 'string') throw new TypeError('NewNodeVersion labelRule must be string');
  if (maybeVer !== undefined && !Maybe.isMaybe(maybeVer)) throw new TypeError('NewNodeVersion version must be Maybe');
  if (maybeNode !== undefined && !Maybe.isMaybe(maybeNode)) throw new TypeError('NewNodeVersion node must be Maybe<TreeNode>');
}

function readPageNodesList(node) {
  // Look for a variable-size property named 'page_nodes' and parse as JSON array
  const infos = node.getPropertyInfos();
  const has = infos.find((p) => p.name === 'page_nodes');
  if (!has) return [];
  try {
    const [, str] = node.getPropertyString('page_nodes');
    const arr = JSON.parse(str);
    if (Array.isArray(arr)) return arr.map(String);
  } catch (_) {
    // ignore parsing errors
  }
  return [];
}
