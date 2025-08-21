// In-memory WWATP server mock that understands WWATP chunked requests and responds
// using the same binary encoding helpers as the client. Used for system-level tests
// with MockCommunication to exercise real request/response flows end-to-end.

import {
  chunkFromWire,
  chunkToWire,
  decodeFromChunks,
  encodeToChunks,
  canDecodeChunks_SequentialNotification,
  decodeChunks_SequentialNotification,
  canDecodeChunks_MaybeTreeNode,
  decodeChunks_MaybeTreeNode,
  canDecodeChunks_VectorTreeNode,
  decodeChunks_VectorTreeNode,
  encodeChunks_label,
  encodeChunks_MaybeTreeNode,
  encodeChunks_VectorTreeNode,
  encodeChunks_VectorSequentialNotification,
  WWATP_SIGNAL,
} from '../../interface/http3_tree_message_helpers.js';
import { Just, Nothing } from '../../interface/maybe.js';

// Utility: parse all chunks in a wire buffer into { signal, requestId, payload }
function parseRequest(bytes) {
  let o = 0;
  const chunks = [];
  while (o < bytes.byteLength) {
    const { chunk, read } = chunkFromWire(bytes.slice(o));
    chunks.push(chunk);
    o += read;
  }
  const signal = chunks[0]?.header?.signal ?? 0;
  const requestId = chunks[0]?.header?.request_id ?? 0;
  const payload = decodeFromChunks(chunks);
  return { signal, requestId, payload, chunks };
}

// Simple notification store shared by all clients for tests
class NotificationStore {
  constructor() {
    this.signalCount = 0;
    this.notifications = []; // Array<{ signalCount, notification: { labelRule, maybeNode } }>
    this.snapshot = new Set(); // last seen set of labelRules
  }
  record(labelRule, maybeNode) {
    this.signalCount += 1;
    this.notifications.push({ signalCount: this.signalCount, notification: { labelRule, maybeNode } });
  }
  getSince(count) {
    return this.notifications.filter((n) => n.signalCount > count);
  }
  // Compute notifications by diffing current backend state vs snapshot.
  syncFromBackend(serverBackend) {
    try {
  const vec = serverBackend.getFullTree?.() || [];
      const current = new Set(vec.map((n) => n.getLabelRule()));
      // Additions or updates: for simplicity, treat all new labels as creations
      for (const n of vec) {
        const lab = n.getLabelRule();
        if (!this.snapshot.has(lab)) {
          this.record(lab, Just(n));
        }
      }
      // Deletions
      for (const lab of Array.from(this.snapshot)) {
        if (!current.has(lab)) {
          this.record(lab, Nothing);
        }
      }
      this.snapshot = current;
    } catch (_) {
      // ignore
    }
  }
}

// Build a handler bound to a server backend implementing the Backend interface (SimpleBackend is fine)
export function createWWATPHandler(serverBackend) {
  const store = new NotificationStore();
  // Initialize snapshot from current server state
  try { store.syncFromBackend(serverBackend); } catch (_) {}

  return function handler(_request, dataBytes) {
    const { signal, requestId, payload, chunks } = parseRequest(dataBytes);
    const respond = (respPayload, respSignal) => {
      const chunks = encodeToChunks(respPayload, { signal: respSignal, requestId });
      // concat
      const parts = chunks.map((c) => chunkToWire(c));
      const total = parts.reduce((s, p) => s + p.byteLength, 0);
      const out = new Uint8Array(total);
      let o = 0; for (const p of parts) { out.set(p, o); o += p.byteLength; }
      return out;
    };

    switch (signal) {
      case WWATP_SIGNAL.SIGNAL_WWATP_GET_NODE_REQUEST: {
        // Request encodes label as a single label chunk; read from chunks directly
        const label = (() => {
          try {
            // First payload chunk after header carries the label
            const c = chunks.find((c) => c.header && typeof c.header.data_length === 'number');
            return new TextDecoder('utf-8').decode(c?.payload || new Uint8Array());
          } catch { return ''; }
        })();
        const maybe = serverBackend.getNode(label);
        const encChunks = encodeChunks_MaybeTreeNode(requestId, WWATP_SIGNAL.SIGNAL_WWATP_GET_NODE_RESPONSE, maybe);
        // Return chunked directly
        const parts = encChunks.map((c) => chunkToWire(c));
        let total = 0; for (const p of parts) total += p.byteLength;
        const out = new Uint8Array(total); let o = 0; for (const p of parts) { out.set(p, o); o += p.byteLength; }
        return out;
      }
      case WWATP_SIGNAL.SIGNAL_WWATP_UPSERT_NODE_REQUEST: {
        const { value: nodes } = decodeChunks_VectorTreeNode(0, chunks);
        const ok = serverBackend.upsertNode(nodes);
        // record notifications per top-level node label
        for (const n of nodes) store.record(n.getLabelRule(), Just(n));
        const enc = new Uint8Array([ok ? 1 : 0]);
        return respond(enc, WWATP_SIGNAL.SIGNAL_WWATP_UPSERT_NODE_RESPONSE);
      }
      case WWATP_SIGNAL.SIGNAL_WWATP_DELETE_NODE_REQUEST: {
        const label = (() => {
          try { const c = chunks.find((c) => c.header && typeof c.header.data_length === 'number'); return new TextDecoder('utf-8').decode(c?.payload || new Uint8Array()); } catch { return ''; }
        })();
        const ok = serverBackend.deleteNode(label);
        store.record(label, Nothing);
        const enc = new Uint8Array([ok ? 1 : 0]);
        return respond(enc, WWATP_SIGNAL.SIGNAL_WWATP_DELETE_NODE_RESPONSE);
      }
      case WWATP_SIGNAL.SIGNAL_WWATP_GET_PAGE_TREE_REQUEST: {
        const label = (() => {
          try { const c = chunks.find((c) => c.header && typeof c.header.data_length === 'number'); return new TextDecoder('utf-8').decode(c?.payload || new Uint8Array()); } catch { return ''; }
        })();
        const vec = serverBackend.getPageTree(label) || [];
        const encChunks = encodeChunks_VectorTreeNode(requestId, WWATP_SIGNAL.SIGNAL_WWATP_GET_PAGE_TREE_RESPONSE, vec);
        const parts = encChunks.map((c) => chunkToWire(c));
        let total = 0; for (const p of parts) total += p.byteLength;
        const out = new Uint8Array(total); let o = 0; for (const p of parts) { out.set(p, o); o += p.byteLength; }
        return out;
      }
      case WWATP_SIGNAL.SIGNAL_WWATP_QUERY_NODES_REQUEST: {
        const label = (() => {
          try { const c = chunks.find((c) => c.header && typeof c.header.data_length === 'number'); return new TextDecoder('utf-8').decode(c?.payload || new Uint8Array()); } catch { return ''; }
        })();
        const vec = serverBackend.queryNodes(label) || [];
        const encChunks = encodeChunks_VectorTreeNode(requestId, WWATP_SIGNAL.SIGNAL_WWATP_QUERY_NODES_RESPONSE, vec);
        const parts = encChunks.map((c) => chunkToWire(c));
        let total = 0; for (const p of parts) total += p.byteLength;
        const out = new Uint8Array(total); let o = 0; for (const p of parts) { out.set(p, o); o += p.byteLength; }
        return out;
      }
      case WWATP_SIGNAL.SIGNAL_WWATP_OPEN_TRANSACTION_LAYER_REQUEST: {
        const enc = new Uint8Array([0]);
        return respond(enc, WWATP_SIGNAL.SIGNAL_WWATP_OPEN_TRANSACTION_LAYER_RESPONSE);
      }
      case WWATP_SIGNAL.SIGNAL_WWATP_CLOSE_TRANSACTION_LAYERS_REQUEST: {
        const enc = new Uint8Array([0]);
        return respond(enc, WWATP_SIGNAL.SIGNAL_WWATP_CLOSE_TRANSACTION_LAYERS_RESPONSE);
      }
      case WWATP_SIGNAL.SIGNAL_WWATP_APPLY_TRANSACTION_REQUEST: {
        const enc = new Uint8Array([0]);
        return respond(enc, WWATP_SIGNAL.SIGNAL_WWATP_APPLY_TRANSACTION_RESPONSE);
      }
      case WWATP_SIGNAL.SIGNAL_WWATP_GET_FULL_TREE_REQUEST: {
        const vec = serverBackend.getFullTree() || [];
  const encChunks = encodeChunks_VectorTreeNode(requestId, WWATP_SIGNAL.SIGNAL_WWATP_GET_FULL_TREE_RESPONSE, vec);
  const parts = encChunks.map((c) => chunkToWire(c));
  let total = 0; for (const p of parts) total += p.byteLength;
  const out = new Uint8Array(total); let o = 0; for (const p of parts) { out.set(p, o); o += p.byteLength; }
  return out;
      }
      case WWATP_SIGNAL.SIGNAL_WWATP_REGISTER_LISTENER_REQUEST: {
        const enc = new Uint8Array([1]);
        return respond(enc, WWATP_SIGNAL.SIGNAL_WWATP_REGISTER_LISTENER_RESPONSE);
      }
      case WWATP_SIGNAL.SIGNAL_WWATP_DEREGISTER_LISTENER_REQUEST: {
        const enc = new Uint8Array([1]);
        return respond(enc, WWATP_SIGNAL.SIGNAL_WWATP_DEREGISTER_LISTENER_RESPONSE);
      }
      case WWATP_SIGNAL.SIGNAL_WWATP_NOTIFY_LISTENERS_REQUEST: {
        // Accept notification and record
        // payload = label + maybeNode
        const label = (() => {
          try { const c = chunks[0]; return new TextDecoder('utf-8').decode(c?.payload || new Uint8Array()); } catch { return ''; }
        })();
        const { value: maybeNode } = decodeChunks_MaybeTreeNode(1, chunks);
        store.record(label, maybeNode);
        const enc = new Uint8Array([1]);
        return respond(enc, WWATP_SIGNAL.SIGNAL_WWATP_NOTIFY_LISTENERS_RESPONSE);
      }
      case WWATP_SIGNAL.SIGNAL_WWATP_PROCESS_NOTIFICATION_REQUEST: {
        const enc = new Uint8Array([1]);
        return respond(enc, WWATP_SIGNAL.SIGNAL_WWATP_PROCESS_NOTIFICATION_RESPONSE);
      }
      case WWATP_SIGNAL.SIGNAL_WWATP_GET_JOURNAL_REQUEST: {
        // Request encodes a SequentialNotification where only signalCount is meaningful.
        // Prefer chunk-based decode; fall back to legacy buffer-based if needed.
        let since = 0;
        try {
          const nxt = canDecodeChunks_SequentialNotification(0, chunks);
          if (nxt) {
            const { value: sn } = decodeChunks_SequentialNotification(0, chunks);
            since = sn.signalCount >>> 0;
          } else {
            since = 0;
          }
        } catch (_) {
          since = 0;
        }
  // Before answering, sync with the authoritative backend so direct changes are observed
  store.syncFromBackend(serverBackend);
  const vec = store.getSince(since);
        const encChunks = encodeChunks_VectorSequentialNotification(requestId, WWATP_SIGNAL.SIGNAL_WWATP_GET_JOURNAL_RESPONSE, vec);
        const parts = encChunks.map((c) => chunkToWire(c));
        let total = 0; for (const p of parts) total += p.byteLength;
        const out = new Uint8Array(total); let o = 0; for (const p of parts) { out.set(p, o); o += p.byteLength; }
        return out;
      }
      default: {
        // Unknown: echo empty OK
        return respond(new Uint8Array(0), WWATP_SIGNAL.SIGNAL_WWATP_RESPONSE_FINAL);
      }
    }
  };
}
