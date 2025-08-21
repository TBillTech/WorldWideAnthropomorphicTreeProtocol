import { describe, it, expect } from 'vitest';
import { MockCommunication, Request, SimpleBackend, Http3ClientBackendUpdater, HTTP3TreeMessage, Http3Helpers, TreeNode, Just } from '../index.js';

const { WWATP_SIGNAL, encodeToChunks, chunkToWire } = Http3Helpers;

function buildResponseForGetNode(_requestBytes) {
  // Build a chunk-based Maybe<TreeNode> = Just(node) response
  const tn = new TreeNode({ labelRule: 'root/test', description: 'Test node', propertyInfos: [], childNames: [], propertyData: new Uint8Array() });
  const chunks = Http3Helpers.encodeChunks_MaybeTreeNode(1, WWATP_SIGNAL.SIGNAL_WWATP_GET_NODE_RESPONSE, Just(tn));
  let total = 0; for (const c of chunks) total += chunkToWire(c).byteLength;
  const out = new Uint8Array(total);
  let o = 0;
  for (const c of chunks) { const w = chunkToWire(c); out.set(w, o); o += w.byteLength; }
  return out;
}

describe('Http3ClientBackendUpdater', () => {
  it('flushes pending request and routes response', async () => {
    const comm = new MockCommunication();
    const updater = new Http3ClientBackendUpdater('t', 'local', 0);
    const local = new SimpleBackend();
    const be = updater.addBackend(local, true, new Request({ path: '/api/wwatp' }), 0);

    // Prepare mock transport: whenever request arrives, return GET_NODE_RESPONSE for a fixed node
    comm.setMockHandler((_req, data) => {
      return buildResponseForGetNode(data);
    });

    // Issue a request through backend
    const p = be.getNode('root/test');

    // Maintain once to flush queue; send + immediate response by mock
    await updater.maintainRequestHandlers(comm, 0);

    const res = await p; // blocking mode resolves
    expect(res.isJust()).toBe(true);
    const n = res.getOrElse(null);
    expect(n.getLabelRule()).toBe('root/test');

    // Verify local cache updated
    const cached = be.localBackend_.getNode('root/test');
    expect(cached.isJust()).toBe(true);
  });
});
