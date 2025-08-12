// In-memory WWATP server mock that understands WWATP chunked requests and responds
// using the same binary encoding helpers as the client. Used for system-level tests
// with MockCommunication to exercise real request/response flows end-to-end.

import {
  chunkFromWire,
  chunkToWire,
  decodeFromChunks,
  encodeToChunks,
  WWATP_SIGNAL,
  encode_label,
  decode_label,
  encode_maybe_tree_node,
  decode_maybe_tree_node,
  encode_vec_tree_node,
  decode_vec_tree_node,
  encode_sequential_notification,
  decode_sequential_notification,
  encode_vec_sequential_notification,
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
  return { signal, requestId, payload };
}

// Simple notification store shared by all clients for tests
class NotificationStore {
  constructor() {
    this.signalCount = 0;
    this.notifications = []; // Array<{ signalCount, notification: { labelRule, maybeNode } }>
  }
  record(labelRule, maybeNode) {
    this.signalCount += 1;
    this.notifications.push({ signalCount: this.signalCount, notification: { labelRule, maybeNode } });
  }
  getSince(count) {
    return this.notifications.filter((n) => n.signalCount > count);
  }
}

// Build a handler bound to a server backend implementing the Backend interface (SimpleBackend is fine)
export function createWWATPHandler(serverBackend) {
  const store = new NotificationStore();

  return function handler(_request, dataBytes) {
    const { signal, requestId, payload } = parseRequest(dataBytes);
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
        const { value: label } = decode_label(payload, 0);
        const maybe = serverBackend.getNode(label);
        const enc = encode_maybe_tree_node(maybe);
        return respond(enc, WWATP_SIGNAL.SIGNAL_WWATP_GET_NODE_RESPONSE);
      }
      case WWATP_SIGNAL.SIGNAL_WWATP_UPSERT_NODE_REQUEST: {
        const { value: nodes } = decode_vec_tree_node(payload, 0);
        const ok = serverBackend.upsertNode(nodes);
        // record notifications per top-level node label
        for (const n of nodes) store.record(n.getLabelRule(), Just(n));
        const enc = new Uint8Array([ok ? 1 : 0]);
        return respond(enc, WWATP_SIGNAL.SIGNAL_WWATP_UPSERT_NODE_RESPONSE);
      }
      case WWATP_SIGNAL.SIGNAL_WWATP_DELETE_NODE_REQUEST: {
        const { value: label } = decode_label(payload, 0);
        const ok = serverBackend.deleteNode(label);
        store.record(label, Nothing);
        const enc = new Uint8Array([ok ? 1 : 0]);
        return respond(enc, WWATP_SIGNAL.SIGNAL_WWATP_DELETE_NODE_RESPONSE);
      }
      case WWATP_SIGNAL.SIGNAL_WWATP_GET_PAGE_TREE_REQUEST: {
        const { value: label } = decode_label(payload, 0);
        const vec = serverBackend.getPageTree(label) || [];
        const enc = encode_vec_tree_node(vec);
        return respond(enc, WWATP_SIGNAL.SIGNAL_WWATP_GET_PAGE_TREE_RESPONSE);
      }
      case WWATP_SIGNAL.SIGNAL_WWATP_QUERY_NODES_REQUEST: {
        const { value: label } = decode_label(payload, 0);
        const vec = serverBackend.queryNodes(label) || [];
        const enc = encode_vec_tree_node(vec);
        return respond(enc, WWATP_SIGNAL.SIGNAL_WWATP_QUERY_NODES_RESPONSE);
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
        const enc = encode_vec_tree_node(vec);
        return respond(enc, WWATP_SIGNAL.SIGNAL_WWATP_GET_FULL_TREE_RESPONSE);
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
        const lab = decode_label(payload, 0);
        const mn = decode_maybe_tree_node(payload, lab.read);
        store.record(lab.value, mn.value);
        const enc = new Uint8Array([1]);
        return respond(enc, WWATP_SIGNAL.SIGNAL_WWATP_NOTIFY_LISTENERS_RESPONSE);
      }
      case WWATP_SIGNAL.SIGNAL_WWATP_PROCESS_NOTIFICATION_REQUEST: {
        const enc = new Uint8Array([1]);
        return respond(enc, WWATP_SIGNAL.SIGNAL_WWATP_PROCESS_NOTIFICATION_RESPONSE);
      }
      case WWATP_SIGNAL.SIGNAL_WWATP_GET_JOURNAL_REQUEST: {
        // Request encodes a SequentialNotification where only signalCount is meaningful
        const { value: seq } = decode_sequential_notification(payload, 0);
        const since = seq.signalCount >>> 0;
        const vec = store.getSince(since);
        const enc = encode_vec_sequential_notification(vec);
        return respond(enc, WWATP_SIGNAL.SIGNAL_WWATP_GET_JOURNAL_RESPONSE);
      }
      default: {
        // Unknown: echo empty OK
        return respond(new Uint8Array(0), WWATP_SIGNAL.SIGNAL_WWATP_RESPONSE_FINAL);
      }
    }
  };
}
