import { describe, it, expect } from 'vitest';
import { MockCommunication, Request, SimpleBackend, Http3ClientBackendUpdater, HTTP3TreeMessage, Http3Helpers, Maybe, Just, Nothing } from '../index.js';

const { WWATP_SIGNAL, encodeToChunks, chunkToWire } = Http3Helpers;

function buildResponseForGetNode(requestBytes) {
  // Decode request bytes minimally to extract requestId and produce a Maybe<TreeNode> with a simple JSON payload
  // Our HTTP3TreeMessage encodes request as chunks; here, we don't strictly decode. We'll accept any input and craft a response for a fixed node.
  // Build a minimal TreeNode JSON consistent with helpers: we can echo an empty Maybe (Nothing) for simplicity.
  // But better: return Just(node) with label "root/test".
  const node = {
    labelRule: 'root/test',
    description: 'Test node',
    propertyInfos: [],
    version: { versionNumber: 1, maxVersionSequence: 256, policy: 'default', authorialProof: { kind: 'nothing' }, authors: { kind: 'nothing' }, readers: { kind: 'nothing' }, collisionDepth: { kind: 'nothing' } },
    childNames: [],
    propertyData: [],
    queryHowTo: { kind: 'nothing' },
    qaSequence: { kind: 'nothing' },
  };
  // Build a fake response message using HTTP3TreeMessage for correct encoding
  const msg = new HTTP3TreeMessage();
  msg.setRequestId(1); // we don't parse, fixed id is okay for mock as handler is per-stream
  // Encode Maybe<TreeNode> response: our helper has only decoder; we can emulate by encoding via encode_maybe_tree_node, but it's not exported separately.
  // Simpler: craft vec<TreeNode> response path and set signal to GET_FULL_TREE_RESPONSE then client will decode accordingly depending. To avoid mismatch, we'll handle Signal explicitly here.
  // Better approach: create chunks manually for GET_NODE_RESPONSE with a JSON TreeNode wrapped as Maybe Just (1 + encoded tree node)
  const json = JSON.stringify({
    labelRule: node.labelRule,
    description: node.description,
    propertyInfos: node.propertyInfos,
    version: node.version,
    childNames: node.childNames,
    propertyData: node.propertyData,
    queryHowTo: node.queryHowTo,
    qaSequence: node.qaSequence,
  });
  const encStr = new TextEncoder().encode(json);
  const payload = new Uint8Array(1 + 4 + encStr.byteLength);
  payload[0] = 1; // Just flag
  const dv = new DataView(payload.buffer);
  dv.setUint32(1, encStr.byteLength >>> 0, true);
  payload.set(encStr, 5);
  const chunks = encodeToChunks(payload, { signal: WWATP_SIGNAL.SIGNAL_WWATP_GET_NODE_RESPONSE, requestId: 1 });
  // Flatten chunks to wire bytes
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
