import { describe, it, expect } from 'vitest';
import { SimpleBackend, Http3Helpers, HTTP3TreeMessage, Just, Nothing, TreeNode } from '../index.js';
import Http3ClientBackend from '../http3_client.js';

function makeNode(label) {
  return new TreeNode({
    labelRule: label,
    description: 'test',
    propertyInfos: [],
    childNames: [],
    propertyData: new Uint8Array(),
  });
}

describe('Http3ClientBackend basics', () => {
  it('queues a getNode request and resolves in blocking mode', async () => {
    const local = new SimpleBackend();
    const request = { scheme: 'https', authority: 'example.com', path: '/wwatp/tree' };
    const backend = new Http3ClientBackend(local, true, request, 0, Nothing);

    // Issue request
    const promise = backend.getNode('animals/lion');
    expect(backend.hasNextRequest()).toBe(true);
    const req = backend.popNextRequest();

  // Simulate server creating a response with the same requestId
    const msg = new HTTP3TreeMessage();
    msg.setRequestId(req.requestId);
    msg.setSignal(Http3Helpers.WWATP_SIGNAL.SIGNAL_WWATP_GET_NODE_RESPONSE);
  // Encode Maybe<TreeNode>(Just(node)) as response payload using chunk-based helper
  msg.responseChunks = Http3Helpers.encodeChunks_MaybeTreeNode(msg.requestId, msg.signal, Just(makeNode('animals/lion')));

    backend.processHTTP3Response(msg);

    const maybeNode = await promise;
    expect(maybeNode.isJust()).toBe(true);
    expect(maybeNode.getOrElse(null).labelRule).toBe('animals/lion');

    // Local cache should be updated
    const cached = local.getNode('animals/lion');
    expect(cached.isJust()).toBe(true);
  });
});
