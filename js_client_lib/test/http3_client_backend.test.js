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
  it('getNode reads from local cache without enqueuing network', async () => {
    const local = new SimpleBackend();
    const request = { scheme: 'https', authority: 'example.com', path: '/wwatp/tree' };
    const backend = new Http3ClientBackend(local, true, request, 0, Nothing);

    // Seed local cache
    const lion = makeNode('animals/lion');
    local.upsertNode([lion]);

    // Non-mutating read should not enqueue any request
    const maybe = backend.getNode('animals/lion');
    expect(backend.hasNextRequest()).toBe(false);
    expect(maybe.isJust()).toBe(true);
    expect(maybe.getOrElse(null).labelRule).toBe('animals/lion');
  });
});
