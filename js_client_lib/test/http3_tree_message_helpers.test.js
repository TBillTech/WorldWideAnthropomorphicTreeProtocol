import { describe, it, expect } from 'vitest';
import {
  SIGNAL_TYPE,
  WWATP_SIGNAL,
  NoChunkHeader,
  SignalChunkHeader,
  PayloadChunkHeader,
  SpanChunk,
  chunkToWire,
  chunkFromWire,
  encode_label,
  decode_label,
  can_decode_label,
  encode_long_string,
  decode_long_string,
  can_decode_long_string,
  encode_tree_node,
  decode_tree_node,
  can_decode_tree_node,
  encode_maybe_tree_node,
  decode_maybe_tree_node,
  can_decode_maybe_tree_node,
  encode_new_node_version,
  decode_new_node_version,
  can_decode_new_node_version,
  encode_sub_transaction,
  decode_sub_transaction,
  can_decode_sub_transaction,
  encode_transaction,
  decode_transaction,
  can_decode_transaction,
  encode_vec_tree_node,
  decode_vec_tree_node,
  can_decode_vec_tree_node,
  encode_sequential_notification,
  decode_sequential_notification,
  can_decode_sequential_notification,
  encode_vec_sequential_notification,
  decode_vec_sequential_notification,
  can_decode_vec_sequential_notification,
  encodeToChunks,
  decodeFromChunks,
} from '../interface/http3_tree_message_helpers.js';
import { TreeNode, TreeNodeVersion } from '../interface/tree_node.js';
import { Just, Nothing } from '../interface/maybe.js';

function makeNode(label) {
  return new TreeNode({
    labelRule: label,
    description: 'd',
    propertyInfos: [{ type: 'bool', name: 'x' }, { type: 'string', name: 's' }],
    version: new TreeNodeVersion({ versionNumber: 1 }),
    childNames: ['a', 'b'],
    propertyData: (() => {
      // bool + string(len+bytes)
      const out = new Uint8Array(1 + 4 + 3);
      const dv = new DataView(out.buffer);
      dv.setUint8(0, 1);
      dv.setUint32(1, 3, true);
      out.set(new TextEncoder().encode('hey'), 5);
      return out;
    })(),
  });
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

describe('string encoders', () => {
  it('label round trip', () => {
    const enc = encode_label('abc');
    expect(can_decode_label(enc)).toBe(true);
    const { value, read } = decode_label(enc);
    expect(value).toBe('abc');
    expect(read).toBe(enc.byteLength);
  });
  it('long string round trip', () => {
    const enc = encode_long_string('hello world');
    expect(can_decode_long_string(enc)).toBe(true);
    const { value, read } = decode_long_string(enc);
    expect(value).toBe('hello world');
    expect(read).toBe(enc.byteLength);
  });
});

describe('TreeNode encoders', () => {
  it('node round trip', () => {
    const node = makeNode('x/y');
    const enc = encode_tree_node(node);
    expect(can_decode_tree_node(enc)).toBe(true);
    const { value } = decode_tree_node(enc);
    expect(value.equals(node)).toBe(true);
  });
  it('maybe node round trip', () => {
    const node = makeNode('x');
    const enc = encode_maybe_tree_node(Just(node));
    expect(can_decode_maybe_tree_node(enc)).toBe(true);
    const { value } = decode_maybe_tree_node(enc);
    expect(value.isJust()).toBe(true);
    expect(value.value.equals(node)).toBe(true);

    const encN = encode_maybe_tree_node(Nothing);
    expect(can_decode_maybe_tree_node(encN)).toBe(true);
    const { value: v2 } = decode_maybe_tree_node(encN);
    expect(v2.isNothing()).toBe(true);
  });
});

describe('transaction encoders', () => {
  it('new node version round trip', () => {
    const node = makeNode('p/q');
    const nnv = [Just(7), ['p/q', Just(node)]];
    const enc = encode_new_node_version(nnv);
    expect(can_decode_new_node_version(enc)).toBe(true);
    const { value } = decode_new_node_version(enc);
    expect(value[0].isJust()).toBe(true);
    expect(value[0].value).toBe(7);
    expect(value[1][0]).toBe('p/q');
    expect(value[1][1].isJust()).toBe(true);
    expect(value[1][1].value.equals(node)).toBe(true);
  });
  it('sub tx and tx round trip', () => {
    const node = makeNode('r');
    const nnv = [Nothing, ['r', Just(node)]];
    const st = [nnv, [nnv, nnv]];
    const encSt = encode_sub_transaction(st);
    expect(can_decode_sub_transaction(encSt)).toBe(true);
    const { value: st2 } = decode_sub_transaction(encSt);
    expect(st2[1].length).toBe(2);

    const tx = [st, st];
    const encTx = encode_transaction(tx);
    expect(can_decode_transaction(encTx)).toBe(true);
    const { value: tx2 } = decode_transaction(encTx);
    expect(tx2.length).toBe(2);
  });
});

describe('vector<TreeNode> encoders', () => {
  it('round trip', () => {
    const nodes = [makeNode('a'), makeNode('b/c')];
    const enc = encode_vec_tree_node(nodes);
    expect(can_decode_vec_tree_node(enc)).toBe(true);
    const { value } = decode_vec_tree_node(enc);
    expect(value.length).toBe(2);
    expect(value[0].equals(nodes[0])).toBe(true);
    expect(value[1].equals(nodes[1])).toBe(true);
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

describe('C++ parity: label/long_string offset + partial decode', () => {
  function expectLabelFailsOnPartialThenPasses(enc) {
    expect(can_decode_label(new Uint8Array(0))).toBe(false);
    for (let i = 0; i < enc.byteLength; i++) {
      const sub = enc.slice(0, i);
      expect(can_decode_label(sub)).toBe(false);
    }
    expect(can_decode_label(enc)).toBe(true);
  }

  it('label mirrors cpp checks', () => {
    const lbls = [
      '',
      'test_label',
      'another_test_label',
      'a_very_long_label_that_exceeds_normal_length',
      'label_with_special_characters_!@#$%^&*()_+',
      'https://example.com/path/to/resource?query=param#fragment',
    ];
    for (const l of lbls) {
  const enc = encode_label(l);
  expectLabelFailsOnPartialThenPasses(enc);
      const { value, read } = decode_label(enc);
      expect(value).toBe(l);
      expect(read).toBe(enc.byteLength);
    }
  });

  function canDecodeGenericWithOffset(encBytes, canFn, decFn) {
    const empty = new Uint8Array(0);
    expect(canFn(empty, 0)).toBe(false);
    const prior = encode_long_string('D'.repeat(5000));
    const combined = new Uint8Array(prior.byteLength + encBytes.byteLength);
    combined.set(prior, 0);
    combined.set(encBytes, prior.byteLength);
    // any prefix shorter than full prior should fail
    for (let cut = 0; cut < encBytes.byteLength; cut++) {
      const sub = combined.slice(0, prior.byteLength + cut);
      expect(canFn(sub, prior.byteLength)).toBe(false);
    }
    expect(canFn(combined, prior.byteLength)).toBe(true);
    const { read } = decFn(encBytes, 0);
    expect(read).toBe(encBytes.byteLength);
  }

  it('long_string mirrors cpp checks', () => {
    const strs = [
      '',
      'short string',
      'a slightly longer string that should still fit in one chunk',
      'string_with_special_characters_!@#$%^&*()_+',
      'https://example.com/path/to/resource?query=param#fragment',
      'a'.repeat(2000),
      'b'.repeat(5000),
    ];
    for (const s of strs) {
      const enc = encode_long_string(s);
      canDecodeGenericWithOffset(enc, can_decode_long_string, decode_long_string);
      const { value } = decode_long_string(enc);
      expect(value).toBe(s);
    }
  });
});

describe('C++ parity: Maybe<TreeNode>', () => {
  function makeAnimalNode(name, dossierTexts = []) {
    const infos = [{ type: 'txt', name: '' }, { type: 'txt', name: '' }].slice(0, dossierTexts.length);
    const dataParts = [];
    for (const txt of dossierTexts) {
      const b = new TextEncoder().encode(txt);
      const u = new Uint8Array(4 + b.byteLength);
      new DataView(u.buffer).setUint32(0, b.byteLength, true);
      u.set(b, 4);
      dataParts.push(u);
    }
    let total = 0; for (const p of dataParts) total += p.byteLength;
    const pd = new Uint8Array(total);
    let o = 0; for (const p of dataParts) { pd.set(p, o); o += p.byteLength; }
    return new TreeNode({ labelRule: name, description: 'desc', propertyInfos: infos, version: new TreeNodeVersion({ versionNumber: 1, maxVersionSequence: 1 }), childNames: [], propertyData: pd, queryHowTo: Just('How to query ' + name), qaSequence: Just(name + ' QA sequence') });
  }

  it('maybe tree node mirrors cpp checks', () => {
    const emptyMaybe = Nothing;
    const encEmpty = encode_maybe_tree_node(emptyMaybe);
    const emptyPrefix = new Uint8Array(0);
    expect(can_decode_maybe_tree_node(emptyPrefix, 0)).toBe(false);
    expect(can_decode_maybe_tree_node(encEmpty, 0)).toBe(true);
    const { value: vEmpty } = decode_maybe_tree_node(encEmpty);
    expect(vEmpty.isNothing()).toBe(true);

    const sponge = makeAnimalNode('Sponge');
    const encSponge = encode_maybe_tree_node(Just(sponge));
    expect(can_decode_maybe_tree_node(encSponge)).toBe(true);
    const { value: vS } = decode_maybe_tree_node(encSponge);
    expect(vS.isJust()).toBe(true);
    expect(vS.value.equals(sponge)).toBe(true);

    const seal = makeAnimalNode('Seal', ['pup 1 dossier', 'pup 2 dossier']);
    const encSeal = encode_maybe_tree_node(Just(seal));
    expect(can_decode_maybe_tree_node(encSeal)).toBe(true);
    const { value: vSeal } = decode_maybe_tree_node(encSeal);
    expect(vSeal.isJust()).toBe(true);
    expect(vSeal.value.equals(seal)).toBe(true);
  });
});

describe('C++ parity: SequentialNotification + Vector<SequentialNotification>', () => {
  function SN(signalCount, labelRule, maybeNode) {
    return { signalCount, notification: { labelRule, maybeNode } };
  }

  it('single sequential notification roundtrip', () => {
    const sn1 = SN(0, '', Nothing);
    const enc1 = encode_sequential_notification(sn1);
    const ok1 = can_decode_sequential_notification(enc1);
    expect(ok1).toBe(true);
    const { value: d1 } = decode_sequential_notification(enc1);
    expect(d1.signalCount).toBe(0);
    expect(d1.notification.labelRule).toBe('');

    const sn2 = SN(10, '/lion/pup_1', Nothing);
    const enc2 = encode_sequential_notification(sn2);
    expect(can_decode_sequential_notification(enc2)).toBe(true);
    const { value: d2 } = decode_sequential_notification(enc2);
    expect(d2.signalCount).toBe(10);
    expect(d2.notification.labelRule).toBe('/lion/pup_1');

    const node = new TreeNode({ labelRule: 'https://example.com/path/to/lion?query=param#fragment', description: 'A complex animal' });
    const sn3 = SN(13, node.labelRule, Just(node));
    const enc3 = encode_sequential_notification(sn3);
    expect(can_decode_sequential_notification(enc3)).toBe(true);
    const { value: d3 } = decode_sequential_notification(enc3);
    expect(d3.signalCount).toBe(13);
    expect(d3.notification.labelRule).toBe(node.labelRule);
    expect(d3.notification.maybeNode.isJust()).toBe(true);
  });

  it('vector<sequential notification> roundtrip', () => {
    const node = new TreeNode({ labelRule: 'https://example.com/path/to/lion?query=param#fragment', description: 'A complex animal' });
    const sponge = new TreeNode({ labelRule: 'Sponge', description: 'Bottom Feeder' });
    const vec = [
      SN(10, '/lion/pup_1', Nothing),
      SN(11, node.labelRule, Just(node)),
      SN(12, '/lion/pup_2', Nothing),
      SN(13, node.labelRule, Just(sponge)),
      SN(14, node.labelRule, Just(node)),
      SN(12, '/lion/pup_2', Nothing),
    ];
    const enc = encode_vec_sequential_notification(vec);
    expect(can_decode_vec_sequential_notification(enc)).toBe(true);
    const { value } = decode_vec_sequential_notification(enc);
    expect(value.length).toBe(vec.length);
    expect(value[0].notification.labelRule).toBe('/lion/pup_1');
    expect(value[1].notification.maybeNode.isJust()).toBe(true);
  });
});
