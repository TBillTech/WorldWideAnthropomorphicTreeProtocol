import { describe, it, expect } from 'vitest';
import { MockCommunication, Request, SimpleBackend, Http3ClientBackendUpdater, HTTP3TreeMessage, Http3Helpers } from '../index.js';
import { TreeNode } from '../index.js';

const { WWATP_SIGNAL, encodeToChunks, chunkToWire } = Http3Helpers;

function buildStaticAssetFinalResponse(node) {
  // Encode as plain TreeNode using helper (long_string), to match client decoders
  const msg = new HTTP3TreeMessage();
  msg.setRequestId(1);
  const enc = Http3Helpers.encode_tree_node(new TreeNode({ labelRule: node.labelRule }));
  // For FINAL, use RESPONSE_FINAL signal with entire payload in one chunk
  const chunks = encodeToChunks(enc, { signal: WWATP_SIGNAL.SIGNAL_WWATP_RESPONSE_FINAL, requestId: 1 });
  let total = 0; for (const c of chunks) total += chunkToWire(c).byteLength;
  const out = new Uint8Array(total);
  let o = 0; for (const c of chunks) { const w = chunkToWire(c); out.set(w, o); o += w.byteLength; }
  return out;
}

describe('Http3ClientBackend static asset flow (non-WWATP path)', () => {
  it('requests static asset and loads staticNode_ from RESPONSE_FINAL', async () => {
    const comm = new MockCommunication();
    const updater = new Http3ClientBackendUpdater('t', 'local', 0);
    const local = new SimpleBackend();
    const request = new Request({ scheme: 'https', authority: 'example.com', path: '/static/node.json' });
    const be = updater.addBackend(local, false, request, 0); // non-blocking mode

    // Prepare mock: reply with RESPONSE_FINAL containing a TreeNode for label 'static/root'
    comm.setMockHandler((_req, data) => {
      // data is ignored; we just return FINAL
      return buildStaticAssetFinalResponse({ labelRule: 'static/root' });
    });

    // Trigger static request via constructor path? requestStaticNodeData is called when staticNode provided.
    // Here, call explicitly.
    be.requestStaticNodeData();

    // Maintain once to send the request and process immediate response
    await updater.maintainRequestHandlers(comm, 0);

    const sn = be.getStaticNode();
    expect(sn && sn.isJust && sn.isJust()).toBe(true);
    const node = sn.getOrElse(null);
    expect(node.getLabelRule()).toBe('static/root');
    // Cached locally as well
    const cached = local.getNode('static/root');
    expect(cached.isJust()).toBe(true);
  });
});
