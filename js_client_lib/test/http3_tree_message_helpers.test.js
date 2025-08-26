import { describe, it, expect } from 'vitest';
import {
  SIGNAL_TYPE,
  WWATP_SIGNAL,
  SignalChunkHeader,
  PayloadChunkHeader,
  SpanChunk,
  chunkToWire,
  chunkFromWire,
  encodeChunks_label,
  encodeChunks_long_string,
  encodeChunks_MaybeTreeNode,
  encodeChunks_VectorTreeNode,
  encodeChunks_NewNodeVersion,
  encodeChunks_SubTransaction,
  encodeChunks_Transaction,
  encodeChunks_SequentialNotification,
  encodeChunks_VectorSequentialNotification,
  decodeChunks_label,
  decodeChunks_long_string,
  decodeChunks_MaybeTreeNode,
  decodeChunks_VectorTreeNode,
  decodeChunks_NewNodeVersion,
  decodeChunks_SubTransaction,
  decodeChunks_Transaction,
  decodeChunks_SequentialNotification,
  decodeChunks_VectorSequentialNotification,
  canDecodeChunks_label,
  canDecodeChunks_long_string,
  canDecodeChunks_MaybeTreeNode,
  canDecodeChunks_VectorTreeNode,
  canDecodeChunks_NewNodeVersion,
  canDecodeChunks_SubTransaction,
  canDecodeChunks_Transaction,
  canDecodeChunks_SequentialNotification,
  canDecodeChunks_VectorSequentialNotification,
  encodeToChunks,
  decodeFromChunks,
} from '../interface/http3_tree_message_helpers.js';

import { TreeNode, TreeNodeVersion } from '../interface/tree_node.js';
import { Just, Nothing } from '../interface/maybe.js';

// Helper: checks that canDecodeChunks returns false/null for all incomplete chunk arrays
function expectCanDecodeChunksRequiresAllChunks(canDecodeChunks, chunks, startChunk = 0) {
  for (let i = 1; i < chunks.length; ++i) {
    const result = canDecodeChunks(startChunk, chunks.slice(0, i));
    expect(!result).toBe(true);
  }
}

function makeTreeNode({
  labelRule = 'node',
  description = 'desc',
  propertyInfos = [{ type: 'int', name: 'foo' }, { type: 'str', name: 'bar' }],
  version = new TreeNodeVersion({ versionNumber: 42, maxVersionSequence: 99, policy: 'custom' }),
  childNames = ['child1', 'child2'],
  properties = [
    { name: 'foo', value: 123 },
    { name: 'bar', value: 'baz' }
  ],
  queryHowTo = Just('howto'),
  qaSequence = Just('qa'),
} = {}) {
  // Omit propertyInfos from the constructor; build up via insert calls
  const node = new TreeNode({ labelRule, description, version, childNames, queryHowTo, qaSequence });
  if (Array.isArray(properties)) {
    for (const prop of properties) {
      const idx = propertyInfos.findIndex(p => p.name === prop.name);
      if (idx === -1) throw new Error(`Property info for '${prop.name}' not found`);
      const type = propertyInfos[idx].type;
      // Use insertProperty for fixed types, insertPropertyString for others
      if (["int64", "uint64", "float", "double", "bool"].includes(type)) {
        node.insertProperty(idx, prop.name, prop.value, type);
      } else if (typeof prop.value === 'string') {
        node.insertPropertyString(idx, prop.name, type, prop.value);
      } else if (prop.value instanceof Uint8Array) {
        node.insertPropertySpan(idx, prop.name, type, prop.value);
      } else {
        // fallback: convert to string
        node.insertPropertyString(idx, prop.name, type, String(prop.value));
      }
    }
  }
  return node;
}

describe('chunk headers', () => {
  it('encodes/decodes signal chunk', () => {
    const c = new SpanChunk(new SignalChunkHeader(42, WWATP_SIGNAL.SIGNAL_WWATP_GET_NODE_REQUEST));
    const wire = chunkToWire(c);
    const { chunk } = chunkFromWire(wire);
    expect(chunk.signalType).toBe(SIGNAL_TYPE.SIGNAL);
    expect(chunk.signal).toBe(WWATP_SIGNAL.SIGNAL_WWATP_GET_NODE_REQUEST);
    expect(chunk.requestId).toBe(42);
  });
  it('encodes/decodes payload chunk', () => {
    const payload = new Uint8Array([1,2,3]);
    const c = new SpanChunk(new PayloadChunkHeader(9, WWATP_SIGNAL.SIGNAL_WWATP_REQUEST_FINAL, payload.byteLength), payload);
    const wire = chunkToWire(c);
    const { chunk } = chunkFromWire(wire);
    expect(chunk.signalType).toBe(SIGNAL_TYPE.PAYLOAD);
    expect(chunk.signal).toBe(WWATP_SIGNAL.SIGNAL_WWATP_REQUEST_FINAL);
    expect(chunk.requestId).toBe(9);
    expect(Array.from(chunk.payload)).toEqual([1,2,3]);
  });
});

describe('string encoders (chunk-based)', () => {
  it('label round trip', () => {
    const enc = encodeChunks_label(1, 2, 'abc');
    expect(canDecodeChunks_label(0, enc)).toBeTruthy();
    const { value } = decodeChunks_label(0, enc);
    expect(value).toBe('abc');
  });
  it('long string round trip', () => {
    const enc = encodeChunks_long_string(1, 2, 'hello world');
    expect(canDecodeChunks_long_string(0, enc)).toBeTruthy();
    const { value } = decodeChunks_long_string(0, enc);
    expect(value).toBe('hello world');
  });
});

describe('MaybeTreeNode chunk-based', () => {
  it('maybe node round trip (simple)', () => {
    const node = makeTreeNode({
      labelRule: 'x',
      properties: [
        { name: 'foo', value: 42 },
        { name: 'bar', value: 'hello' }
      ]
    });
    const chunks = encodeChunks_MaybeTreeNode(1, 2, Just(node));
    expect(canDecodeChunks_MaybeTreeNode(0, chunks)).toBeTruthy();
    expectCanDecodeChunksRequiresAllChunks(canDecodeChunks_MaybeTreeNode, chunks);
    const { value } = decodeChunks_MaybeTreeNode(0, chunks);
    expect(value.isJust()).toBe(true);
    const decoded = value.getOrElse(null);
    expect(decoded.labelRule).toBe(node.labelRule);
    expect(decoded.description).toBe(node.description);
    expect(decoded.getPropertyData().toString()).toBe(node.getPropertyData().toString());
    expect(decoded.childNames).toEqual(node.childNames);
    expect(decoded.propertyInfos).toEqual(node.propertyInfos);
    expect(decoded.version.versionNumber).toBe(node.version.versionNumber);
    expect(decoded.version.maxVersionSequence).toBe(node.version.maxVersionSequence);
    expect(decoded.version.policy).toBe(node.version.policy);
    expect(typeof decoded.getQueryHowTo()).toBe('object');
    expect(typeof decoded.getQaSequence()).toBe('object');

    const chunksN = encodeChunks_MaybeTreeNode(1, 2, Nothing);
    expect(canDecodeChunks_MaybeTreeNode(0, chunksN)).toBeTruthy();
    expectCanDecodeChunksRequiresAllChunks(canDecodeChunks_MaybeTreeNode, chunksN);
    const { value: v2 } = decodeChunks_MaybeTreeNode(0, chunksN);
    expect(v2.isNothing()).toBe(true);
  });

  it('maybe node round trip (complex)', () => {
    const node = makeTreeNode({
      labelRule: 'complex/label',
      description: 'A complex node',
      propertyInfos: [
        { type: 'float', name: 'pi' },
        { type: 'bool', name: 'flag' },
      ],
      version: new TreeNodeVersion({ versionNumber: 7, maxVersionSequence: 77, policy: 'strict' }),
      childNames: ['alpha', 'beta', 'gamma'],
      properties: [
        { name: 'pi', value: 3.14159 },
        { name: 'flag', value: true }
      ],
      queryHowTo: Just('complexHow'),
      qaSequence: Just('complexQA'),
    });
    const chunks = encodeChunks_MaybeTreeNode(1, 2, Just(node));
    expect(canDecodeChunks_MaybeTreeNode(0, chunks)).toBeTruthy();
    expectCanDecodeChunksRequiresAllChunks(canDecodeChunks_MaybeTreeNode, chunks);
    const { value } = decodeChunks_MaybeTreeNode(0, chunks);
    expect(value.isJust()).toBe(true);
    const decoded = value.getOrElse(null);
    expect(decoded.labelRule).toBe(node.labelRule);
    expect(decoded.description).toBe(node.description);
    expect(decoded.getPropertyData().toString()).toBe(node.getPropertyData().toString());
    expect(decoded.childNames).toEqual(node.childNames);
    expect(decoded.propertyInfos).toEqual(node.propertyInfos);
    expect(decoded.version.versionNumber).toBe(node.version.versionNumber);
    expect(decoded.version.maxVersionSequence).toBe(node.version.maxVersionSequence);
    expect(decoded.version.policy).toBe(node.version.policy);
    expect(typeof decoded.getQueryHowTo()).toBe('object');
    expect(typeof decoded.getQaSequence()).toBe('object');
  });
});

describe('transaction encoders (chunk-based)', () => {
  it('new node version round trip', () => {
    const nnv = [1, ['label', null]];
    const chunks = encodeChunks_NewNodeVersion(1, 2, nnv);
    expect(canDecodeChunks_NewNodeVersion(0, chunks)).toBeTruthy();
    expectCanDecodeChunksRequiresAllChunks(canDecodeChunks_NewNodeVersion, chunks);
    const { value } = decodeChunks_NewNodeVersion(0, chunks);
  // value[0] is a Maybe, unwrap for comparison
  expect(value[0].isJust ? value[0].getOrElse(null) : value[0]).toBe(1);
  expect(value[1][0]).toBe('label');
  });
  it('sub tx and tx round trip', () => {
    const nnv = [1, ['label', null]];
    const subTx = [nnv, [nnv, nnv]];
    const tx = [subTx, subTx];
    const chunks = encodeChunks_Transaction(1, 2, tx);
    expect(canDecodeChunks_Transaction(0, chunks)).toBeTruthy();
    expectCanDecodeChunksRequiresAllChunks(canDecodeChunks_Transaction, chunks);
    const { value } = decodeChunks_Transaction(0, chunks);
    expect(value.length).toBe(2);
    expect(value[0][1].length).toBe(2);
  });
});

describe('vector<TreeNode> encoders (chunk-based)', () => {
  it('round trip', () => {
    const nodes = [
      makeTreeNode({
        labelRule: 'a',
        description: 'descA',
        propertyInfos: [{ type: 'int', name: 'foo' }],
        properties: [{ name: 'foo', value: 1 }]
      }),
      makeTreeNode({
        labelRule: 'b/c',
        description: 'descB',
        propertyInfos: [{ type: 'int', name: 'bar' }],
        properties: [{ name: 'bar', value: 5 }]
      }),
    ];
    const chunks = encodeChunks_VectorTreeNode(1, 2, nodes);
    expect(canDecodeChunks_VectorTreeNode(0, chunks)).toBeTruthy();
    expectCanDecodeChunksRequiresAllChunks(canDecodeChunks_VectorTreeNode, chunks);
    const { value } = decodeChunks_VectorTreeNode(0, chunks);
    expect(value.length).toBe(2);
    for (let i = 0; i < nodes.length; ++i) {
      expect(value[i].labelRule).toBe(nodes[i].labelRule);
      expect(value[i].description).toBe(nodes[i].description);
      expect(value[i].getPropertyData().toString()).toBe(nodes[i].getPropertyData().toString());
    }
  });
});

describe('chunk flatten/collect', () => {
  it('splits and recombines payloads', () => {
    const payload = new TextEncoder().encode('x'.repeat(3000));
    const chunks = encodeToChunks(payload, { signal: WWATP_SIGNAL.SIGNAL_WWATP_REQUEST_CONTINUE, requestId: 5, maxChunkSize: 600 });
    expect(chunks.length).toBeGreaterThan(1);
    const collected = decodeFromChunks(chunks);
    expect(collected.byteLength).toBe(payload.byteLength);
    expect(new TextDecoder().decode(collected)).toBe('x'.repeat(3000));
  });
});

describe('SequentialNotification encoders (chunk-based)', () => {
  it('round trip', () => {
    const node = makeTreeNode({ labelRule: 'notif', description: 'notify', propertyInfos: [{ type: 'int', name: 'foo' }], properties: [{ name: 'foo', value: 99 }] });
    const sn = { signalCount: 42, notification: { labelRule: node.labelRule, maybeNode: Just(node) } };
    const chunks = encodeChunks_SequentialNotification(1, 2, sn);
    expect(canDecodeChunks_SequentialNotification(0, chunks)).toBeTruthy();
    expectCanDecodeChunksRequiresAllChunks(canDecodeChunks_SequentialNotification, chunks);
    const { value } = decodeChunks_SequentialNotification(0, chunks);
    expect(value.signalCount).toBe(sn.signalCount);
    expect(value.notification.labelRule).toBe(sn.notification.labelRule);
    expect(value.notification.maybeNode.isJust()).toBe(true);
    expect(value.notification.maybeNode.getOrElse(null).labelRule).toBe(node.labelRule);
  });

  it('decodes delete notification (Nothing)', () => {
    // Deletion: valid signalCount and labelRule, maybeNode = Nothing
    const sn = { signalCount: 99, notification: { labelRule: 'lion', maybeNode: Nothing } };
    const chunks = encodeChunks_SequentialNotification(7, WWATP_SIGNAL.SIGNAL_WWATP_GET_JOURNAL_RESPONSE, sn);
    expect(canDecodeChunks_SequentialNotification(0, chunks)).toBeTruthy();
    expectCanDecodeChunksRequiresAllChunks(canDecodeChunks_SequentialNotification, chunks);
    const { value } = decodeChunks_SequentialNotification(0, chunks);
    expect(value.signalCount).toBe(99);
    expect(value.notification.labelRule).toBe('lion');
    expect(value.notification.maybeNode.isNothing()).toBe(true);
  });
});

describe('VectorSequentialNotification encoders (chunk-based)', () => {
  it('round trip', () => {
    const node1 = makeTreeNode({ labelRule: 'n1', description: 'n1desc', propertyInfos: [{ type: 'int', name: 'foo' }], properties: [{ name: 'foo', value: 1 }] });
    const node2 = makeTreeNode({ labelRule: 'n2', description: 'n2desc', propertyInfos: [{ type: 'int', name: 'bar' }], properties: [{ name: 'bar', value: 2 }] });
    const vec = [
      { signalCount: 1, notification: { labelRule: node1.labelRule, maybeNode: Just(node1) } },
      { signalCount: 2, notification: { labelRule: node2.labelRule, maybeNode: Just(node2) } }
    ];
    const chunks = encodeChunks_VectorSequentialNotification(1, 2, vec);
    expect(canDecodeChunks_VectorSequentialNotification(0, chunks)).toBeTruthy();
    expectCanDecodeChunksRequiresAllChunks(canDecodeChunks_VectorSequentialNotification, chunks);
    const { value } = decodeChunks_VectorSequentialNotification(0, chunks);
    expect(value.length).toBe(2);
    for (let i = 0; i < vec.length; ++i) {
      expect(value[i].signalCount).toBe(vec[i].signalCount);
      expect(value[i].notification.labelRule).toBe(vec[i].notification.labelRule);
      expect(value[i].notification.maybeNode.isJust()).toBe(true);
      expect(value[i].notification.maybeNode.getOrElse(null).labelRule).toBe(vec[i].notification.maybeNode.getOrElse(null).labelRule);
    }
  });
});

describe('SubTransaction encoders (chunk-based)', () => {
  it('round trip', () => {
    const node = makeTreeNode({ labelRule: 'main', description: 'main', propertyInfos: [{ type: 'int', name: 'foo' }], properties: [{ name: 'foo', value: 1 }] });
    const child = makeTreeNode({ labelRule: 'child', description: 'child', propertyInfos: [{ type: 'int', name: 'bar' }], properties: [{ name: 'bar', value: 2 }] });
    const mainNNV = [Just(1), [node.labelRule, Just(node)]];
    const descNNV = [Nothing, [child.labelRule, Just(child)]];
    const subTx = [mainNNV, [descNNV]];
    const chunks = encodeChunks_SubTransaction(1, 2, subTx);
    expect(canDecodeChunks_SubTransaction(0, chunks)).toBeTruthy();
    expectCanDecodeChunksRequiresAllChunks(canDecodeChunks_SubTransaction, chunks);
    const { value } = decodeChunks_SubTransaction(0, chunks);
    expect(Array.isArray(value)).toBe(true);
    expect(value[0][1][0]).toBe(node.labelRule);
    expect(value[1][0][1][0]).toBe(child.labelRule);
  });
});

describe('Transaction encoders (chunk-based)', () => {
  it('round trip', () => {
    const node1 = makeTreeNode({ labelRule: 'main1', description: 'main1', propertyInfos: [{ type: 'int', name: 'foo' }], properties: [{ name: 'foo', value: 1 }] });
    const child1 = makeTreeNode({ labelRule: 'child1', description: 'child1', propertyInfos: [{ type: 'int', name: 'bar' }], properties: [{ name: 'bar', value: 2 }] });
    const node2 = makeTreeNode({ labelRule: 'main2', description: 'main2', propertyInfos: [{ type: 'int', name: 'baz' }], properties: [{ name: 'baz', value: 3 }] });
    const child2 = makeTreeNode({ labelRule: 'child2', description: 'child2', propertyInfos: [{ type: 'int', name: 'qux' }], properties: [{ name: 'qux', value: 4 }] });
    const tx = [
      [ [Just(1), [node1.labelRule, Just(node1)]], [ [Nothing, [child1.labelRule, Just(child1)]] ] ],
      [ [Just(2), [node2.labelRule, Just(node2)]], [ [Nothing, [child2.labelRule, Just(child2)]] ] ]
    ];
    const chunks = encodeChunks_Transaction(1, 2, tx);
    expect(canDecodeChunks_Transaction(0, chunks)).toBeTruthy();
    expectCanDecodeChunksRequiresAllChunks(canDecodeChunks_Transaction, chunks);
    const { value } = decodeChunks_Transaction(0, chunks);
    expect(Array.isArray(value)).toBe(true);
    expect(value.length).toBe(2);
    expect(value[0][0][1][0]).toBe(node1.labelRule);
    expect(value[0][1][0][1][0]).toBe(child1.labelRule);
    expect(value[1][0][1][0]).toBe(node2.labelRule);
    expect(value[1][1][0][1][0]).toBe(child2.labelRule);
  });
});
