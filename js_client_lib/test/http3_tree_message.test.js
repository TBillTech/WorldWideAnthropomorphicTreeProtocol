import { describe, it, expect } from 'vitest';
import HTTP3TreeMessage from '../interface/http3_tree_message.js';
import { WWATP_SIGNAL } from '../interface/http3_tree_message_helpers.js';
import { TreeNode, TreeNodeVersion } from '../interface/tree_node.js';

function u8(n) { return new Uint8Array(n); }

describe('HTTP3TreeMessage basics', () => {
  it('encodes getNode request', () => {
    const msg = new HTTP3TreeMessage().setRequestId(123);
    msg.encodeGetNodeRequest('animals/lion');
    expect(msg.hasNextRequestChunk()).toBe(true);
    const wire = msg.nextRequestWireBytes();
    expect(wire[0]).toBe(2); // payload header
  });

  it('encodes upsertNode request', () => {
    const n = new TreeNode({ labelRule: 'a', description: 'x', version: new TreeNodeVersion({ versionNumber: 1 }) });
    const msg = new HTTP3TreeMessage().setRequestId(2);
    msg.encodeUpsertNodeRequest([n]);
    const wire = msg.nextRequestWireBytes();
    expect(wire[0]).toBe(2);
  });
});
