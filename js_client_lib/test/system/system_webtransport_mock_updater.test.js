import { describe, it, expect } from 'vitest';
import { WebTransportCommunication, Request, Http3ClientBackendUpdater, SimpleBackend, TreeNode, TreeNodeVersion, Just, Nothing } from '../../index.js';
import {
  WWATP_SIGNAL,
  chunkFromWire,
  chunkToWire,
  collectPayloadBytes,
  encode_maybe_tree_node,
  encode_vec_tree_node,
  encodeToChunks,
  decode_label,
  decode_vec_tree_node,
} from '../../interface/http3_tree_message_helpers.js';

// A WebTransport polyfill that invokes a provided handler with the request bytes
// and streams back the handler's response bytes when writer.close() is called.
class MockServerWebTransport {
  constructor(_url) {
    this.ready = Promise.resolve();
    this._closedResolve = null;
    this.closed = new Promise((res) => { this._closedResolve = res; });
  }
  async close() { this._closedResolve?.(); }
  static setHandler(fn) { MockServerWebTransport._handler = fn; }
  async createBidirectionalStream() {
    const state = { chunks: [], closed: false, responded: false, response: new Uint8Array(0) };
    const writer = {
      async write(chunk) { state.chunks.push(chunk instanceof Uint8Array ? chunk : new Uint8Array(chunk)); },
      async close() {
        state.closed = true;
        // Concatenate request bytes
        let total = 0; for (const c of state.chunks) total += c.byteLength;
        const req = new Uint8Array(total); let o = 0; for (const c of state.chunks) { req.set(c, o); o += c.byteLength; }
        // Compute response via handler
        const res = await (MockServerWebTransport._handler?.(req) ?? new Uint8Array(0));
        state.response = res instanceof Uint8Array ? res : (res ? new Uint8Array(res) : new Uint8Array(0));
        state.responded = true;
      },
      async abort() { state.closed = true; },
      releaseLock() {},
    };
    const reader = {
      async read() {
        if (!state.closed || !state.responded) { return { value: undefined, done: false }; }
        if (state.response) {
          const v = state.response; state.response = null; return { value: v, done: false };
        }
        return { value: undefined, done: true };
      },
      async cancel() { state.closed = true; },
      releaseLock() {},
    };
    return { writable: { getWriter: () => writer }, readable: { getReader: () => reader } };
  }
}

describe('System (WebTransport mock) – updater E2E', () => {
  it('upsert a node and fetch it back via WebTransportCommunication + Updater', async () => {
    // Install polyfill
    const orig = globalThis.WebTransport;
    globalThis.WebTransport = MockServerWebTransport;

    // In-memory node store
    const store = new Map();

    // Helper to concatenate chunk wires
    function buildResponseBytes(chunks) {
      let total = 0; for (const c of chunks) total += c.byteLength;
      const out = new Uint8Array(total); let o = 0; for (const c of chunks) { out.set(c, o); o += c.byteLength; }
      return out;
    }

    // Server-side request handler
    MockServerWebTransport.setHandler((reqBytes) => {
      // Parse first chunk to get request id and signal
      let chunks = [];
      let i = 0;
      while (i < reqBytes.byteLength) {
        const { chunk, read } = chunkFromWire(reqBytes.slice(i));
        chunks.push(chunk); i += read;
      }
      if (chunks.length === 0) return new Uint8Array(0);
      // Find first non-heartbeat payload
      const first = chunks.find((c) => c.header?.signal !== WWATP_SIGNAL.SIGNAL_HEARTBEAT) || chunks[0];
      const rid = first.header?.request_id || 1;
      const sig = first.header?.signal || 0;
      const payload = collectPayloadBytes(chunks);
    switch (sig) {
        case WWATP_SIGNAL.SIGNAL_WWATP_UPSERT_NODE_REQUEST: {
          const { value: nodes } = decode_vec_tree_node(payload, 0);
          for (const n of nodes) store.set(n.getLabelRule(), n);
      const ok = new TextEncoder().encode('1');
          const parts = encodeToChunks(ok, { signal: WWATP_SIGNAL.SIGNAL_WWATP_UPSERT_NODE_RESPONSE, requestId: rid });
          return buildResponseBytes(parts.map(chunkToWire));
        }
        case WWATP_SIGNAL.SIGNAL_WWATP_GET_NODE_REQUEST: {
          const { value: label } = decode_label(payload, 0);
          const node = store.get(label) || null;
          const maybe = node ? Just(node) : Nothing;
          const bytes = encode_maybe_tree_node(maybe);
          const parts = encodeToChunks(bytes, { signal: WWATP_SIGNAL.SIGNAL_WWATP_GET_NODE_RESPONSE, requestId: rid });
          return buildResponseBytes(parts.map(chunkToWire));
        }
        default: {
          // Default: echo back non-empty ack
          const ok = new TextEncoder().encode('1');
          const parts = encodeToChunks(ok, { signal: WWATP_SIGNAL.SIGNAL_WWATP_RESPONSE_FINAL, requestId: rid });
          return buildResponseBytes(parts.map(chunkToWire));
        }
      }
    });

    try {
      // Client side: WebTransportCommunication
      const comm = new WebTransportCommunication('https://mock/init/wwatp/');
      await comm.connect();

      // Updater + backend
      const updater = new Http3ClientBackendUpdater('mock', '127.0.0.1', 12345);
      const local = new SimpleBackend();
      const be = updater.addBackend(
        local,
        true,
        new Request({ scheme: 'https', authority: 'mock', path: '/init/wwatp/' }),
        60
      );
      updater.start(comm, 0, 20);

      // Create node and upsert
      const node = new TreeNode({
        labelRule: 'e2e_wt_mock_node',
        description: 'hello mock',
        propertyInfos: [],
        version: new TreeNodeVersion({ versionNumber: 1, maxVersionSequence: 256, policy: 'public' }),
        childNames: [],
      });
      node.insertProperty(0, 'popularity', 7n, 'uint64');
      node.insertPropertyString(1, 'diet', 'string', 'omnivore');

      const ok = await be.upsertNode([node]);
      expect(ok).toBe(true);

      const maybe = await be.getNode('e2e_wt_mock_node');
      expect(maybe && maybe.isJust && maybe.isJust()).toBe(true);
      const got = maybe.getOrElse(null);
      expect(got.getDescription()).toBe('hello mock');

      updater.stop();
      await comm.close();
    } finally {
      // Restore polyfill
      try { if (orig) globalThis.WebTransport = orig; else delete globalThis.WebTransport; } catch {}
    }
  }, 10000);
});
