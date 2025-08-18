import { describe, it, expect } from 'vitest';
import HTTP3TreeMessage from '../interface/http3_tree_message.js';
import {
  WWATP_SIGNAL,
  decode_label,
  encode_vec_tree_node,
  decode_vec_tree_node,
  encode_maybe_tree_node,
  decode_maybe_tree_node,
  decode_transaction,
  decode_sequential_notification,
  encode_vec_sequential_notification,
  decodeFromChunks,
  encodeToChunks,
} from '../interface/http3_tree_message_helpers.js';
import { TreeNode, TreeNodeVersion } from '../interface/tree_node.js';
import { Just, Nothing } from '../interface/maybe.js';
import { createAnimalNode } from './backend_testbed/backend_testbed.js';

function nodeEq(a, b) { return a instanceof TreeNode && b instanceof TreeNode && a.equals(b); }

function expectMaybeTreeNodeEq(actualMaybe, expectedMaybe) {
  if (expectedMaybe.isNothing()) {
    expect(actualMaybe.isNothing()).toBe(true);
  } else {
    expect(actualMaybe.isJust()).toBe(true);
    expect(nodeEq(actualMaybe.value, expectedMaybe.value)).toBe(true);
  }
}

function toBoolBytes(v) { return new Uint8Array([v ? 1 : 0]); }

describe('HTTP3TreeMessage basics', () => {
  it('encodes getNode request (smoke)', () => {
    const msg = new HTTP3TreeMessage().setRequestId(123);
    msg.encodeGetNodeRequest('animals/lion');
    expect(msg.hasNextRequestChunk()).toBe(true);
    const wire = msg.nextRequestWireBytes();
    expect(wire[0]).toBe(2); // payload header
  });

  it('encodes upsertNode request (smoke)', () => {
    const n = new TreeNode({ labelRule: 'a', description: 'x', version: new TreeNodeVersion({ versionNumber: 1 }) });
    const msg = new HTTP3TreeMessage().setRequestId(2);
    msg.encodeUpsertNodeRequest([n]);
    const wire = msg.nextRequestWireBytes();
    expect(wire[0]).toBe(2);
  });
});

// Generic message cycle helper like the C++ testbed
function messageCycleTest({
  request_id,
  request_obj,
  response_obj,
  encode_request_func,
  decode_request_func,
  encode_response_func,
  decode_response_func,
  response_signal,
}) {
  // Client encodes request
  const clientMsg = new HTTP3TreeMessage().setRequestId(request_id);
  encode_request_func(clientMsg, request_obj);
  expect(clientMsg.hasNextRequestChunk()).toBe(true);
  const firstWire = clientMsg.nextRequestWireBytes();
  expect(firstWire[0]).toBe(2); // payload header on wire

  // Server decodes request
  const reqBytes = decodeFromChunks(clientMsg.requestChunks);
  const decodedRequestObj = decode_request_func(reqBytes);
  // Basic equality checks (deep for arrays and TreeNodes)
  if (Array.isArray(request_obj) && request_obj.length && request_obj[0] instanceof TreeNode) {
    expect(decodedRequestObj.length).toBe(request_obj.length);
    decodedRequestObj.forEach((n, i) => expect(n.equals(request_obj[i])).toBe(true));
  } else if (request_obj instanceof TreeNode) {
    expect(decodedRequestObj.equals(request_obj)).toBe(true);
  } else {
    expect(decodedRequestObj).toEqual(request_obj);
  }

  // Server encodes response
  const respPayload = encode_response_func(response_obj);
  const respChunks = encodeToChunks(respPayload, { signal: response_signal, requestId: request_id });

  // Client receives and decodes response
  clientMsg.responseChunks = respChunks;
  const decodedResponseObj = decode_response_func(clientMsg);

  // Compare
  if (Array.isArray(response_obj) && response_obj.length && response_obj[0] instanceof TreeNode) {
    expect(decodedResponseObj.length).toBe(response_obj.length);
    decodedResponseObj.forEach((n, i) => expect(n.equals(response_obj[i])).toBe(true));
  } else if (response_obj && response_obj.kind && (response_obj.kind === 'just' || response_obj.kind === 'nothing')) {
    expectMaybeTreeNodeEq(decodedResponseObj, response_obj);
  } else {
    expect(decodedResponseObj).toEqual(response_obj);
  }
}

describe('HTTP3TreeMessage encode/decode getNodeRequest', () => {
  function nodeRequestTest(request_id, request_label, response_maybe_node) {
    const encode_request_func = (clientMsg, reqLabel) => clientMsg.encodeGetNodeRequest(reqLabel);
    const decode_request_func = (bytes) => {
      const { value } = decode_label(bytes, 0);
      return value;
    };
    const encode_response_func = (maybeNode) => encode_maybe_tree_node(maybeNode);
    const decode_response_func = (clientMsg) => clientMsg.decodeGetNodeResponse();

    messageCycleTest({
      request_id,
      request_obj: request_label,
      response_obj: response_maybe_node,
      encode_request_func,
      decode_request_func,
      encode_response_func,
      decode_response_func,
      request_signal: WWATP_SIGNAL.SIGNAL_WWATP_GET_NODE_REQUEST,
      response_signal: WWATP_SIGNAL.SIGNAL_WWATP_GET_NODE_RESPONSE,
    });
  }

  it('round-trips various labels and maybe nodes', () => {
    let request_id = 6;
    const anAnimal = createAnimalNode(
      'Seal',
      'A marine mammal',
      [{ type: 'txt', name: '' }, { type: 'txt', name: '' }],
      { versionNumber: 1, maxVersionSequence: 1 },
      ['pup_1', 'pup_2'],
      ['pup 1 dossier', 'pup 2 dossier'],
      'How to query seal',
      'Seal QA sequence'
    );

    nodeRequestTest(request_id++, 'test_label', Just(anAnimal));
    nodeRequestTest(request_id++, '', Just(anAnimal));
    nodeRequestTest(request_id++, '/lion/cub_1', Nothing);
    nodeRequestTest(request_id++, 'https://example.com/path/to/resource?query=param#fragment', Just(anAnimal));
  });
});

describe('HTTP3TreeMessage encode/decode upsertNodeRequest', () => {
  function nodeUpsertTest(request_id, nodes, response_bool) {
    const encode_request_func = (clientMsg, ns) => clientMsg.encodeUpsertNodeRequest(ns);
    const decode_request_func = (bytes) => {
      const { value } = decode_vec_tree_node(bytes, 0);
      return value;
    };
    const encode_response_func = (b) => toBoolBytes(b);
    const decode_response_func = (clientMsg) => clientMsg.decodeUpsertNodeResponse();

    messageCycleTest({
      request_id,
      request_obj: nodes,
      response_obj: response_bool,
      encode_request_func,
      decode_request_func,
      encode_response_func,
      decode_response_func,
      request_signal: WWATP_SIGNAL.SIGNAL_WWATP_UPSERT_NODE_REQUEST,
      response_signal: WWATP_SIGNAL.SIGNAL_WWATP_UPSERT_NODE_RESPONSE,
    });
  }

  it('round-trips node vectors and bool responses', () => {
    let request_id = 6;
    const complex_node = createAnimalNode(
      'https://example.com/path/to/lion?query=param#fragment',
      'A complex animal',
      [{ type: 'txt', name: 'pup_1_dossier' }, { type: 'txt', name: 'pup_2_dossier' }],
      { versionNumber: 2, maxVersionSequence: 2 },
      ['pup_1', 'pup_2'],
      ['pup 1 dossier', 'pup 2 dossier'],
      'How to query complex animal',
      'Complex Animal QA sequence'
    );
    const simpleAnimal = createAnimalNode('Sponge', 'Bottom Feeder', [], { versionNumber: 1, maxVersionSequence: 1 }, [], [], '', '');
    const anAnimal = createAnimalNode(
      'Seal',
      'A marine mammal',
      [{ type: 'txt', name: '' }, { type: 'txt', name: '' }],
      { versionNumber: 1, maxVersionSequence: 1 },
      ['pup_1', 'pup_2'],
      ['pup 1 dossier', 'pup 2 dossier'],
      'How to query seal',
      'Seal QA sequence'
    );

    nodeUpsertTest(request_id++, [complex_node], true);
    nodeUpsertTest(request_id++, [simpleAnimal], true);
    nodeUpsertTest(request_id++, [simpleAnimal, complex_node, anAnimal], true);
    nodeUpsertTest(request_id++, [], false);
    nodeUpsertTest(request_id++, [simpleAnimal], false);
  });
});

// The following parity suites will be added as HTTP3TreeMessage gains methods.
// They are scaffolded as TODOs to mirror the C++ tests in test_instances/catch2_unit_tests/http3_message_tests.cpp
describe('HTTP3TreeMessage parity scaffolds (TODO)', () => {
  it('encode and decode deleteNodeRequest', () => {
    function nodeDeleteTest(request_id, req_label, resp_bool) {
      const msg = new HTTP3TreeMessage().setRequestId(request_id);
      msg.encodeDeleteNodeRequest(req_label);
      const reqBytes = decodeFromChunks(msg.requestChunks);
      const { value: decodedLabel } = decode_label(reqBytes, 0);
      expect(decodedLabel).toEqual(req_label);
      const respChunks = encodeToChunks(new Uint8Array([resp_bool ? 1 : 0]), { signal: WWATP_SIGNAL.SIGNAL_WWATP_DELETE_NODE_RESPONSE, requestId: request_id });
      msg.responseChunks = respChunks;
      expect(msg.decodeDeleteNodeResponse()).toBe(resp_bool);
    }

    let request_id = 6;
    nodeDeleteTest(request_id++, 'test_label', true);
    nodeDeleteTest(request_id++, '', true);
    nodeDeleteTest(request_id++, '/lion/cub_1', false);
    nodeDeleteTest(request_id++, 'https://example.com/path/to/resource?query=param#fragment', true);
  });

  it('encode and decode getPageTreeRequest', () => {
    function pageTreeRequestTest(request_id, req_label, resp_nodes) {
      const msg = new HTTP3TreeMessage().setRequestId(request_id);
      msg.encodeGetPageTreeRequest(req_label);
      const reqBytes = decodeFromChunks(msg.requestChunks);
      const { value: decodedLabel } = decode_label(reqBytes, 0);
      expect(decodedLabel).toEqual(req_label);
      const respPayload = encode_vec_tree_node(resp_nodes);
      msg.responseChunks = encodeToChunks(respPayload, { signal: WWATP_SIGNAL.SIGNAL_WWATP_GET_PAGE_TREE_RESPONSE, requestId: request_id });
      const out = msg.decodeGetPageTreeResponse();
      expect(out.length).toBe(resp_nodes.length);
      out.forEach((n, i) => expect(n.equals(resp_nodes[i])).toBe(true));
    }

    let request_id = 6;
    const complex_node = createAnimalNode(
      'https://example.com/path/to/lion?query=param#fragment',
      'A complex animal',
      [{ type: 'txt', name: 'pup_1_dossier' }, { type: 'txt', name: 'pup_2_dossier' }],
      { versionNumber: 2, maxVersionSequence: 2 },
      ['pup_1', 'pup_2'],
      ['pup 1 dossier', 'pup 2 dossier'],
      'How to query complex animal',
      'Complex Animal QA sequence'
    );
    const simpleAnimal = createAnimalNode('Sponge', 'Bottom Feeder', [], { versionNumber: 1, maxVersionSequence: 1 }, [], [], '', '');
    const anAnimal = createAnimalNode(
      'Seal',
      'A marine mammal',
      [{ type: 'txt', name: '' }, { type: 'txt', name: '' }],
      { versionNumber: 1, maxVersionSequence: 1 },
      ['pup_1', 'pup_2'],
      ['pup 1 dossier', 'pup 2 dossier'],
      'How to query seal',
      'Seal QA sequence'
    );

    pageTreeRequestTest(request_id++, 'https://example.com/path/to/animal_page', [anAnimal]);
    pageTreeRequestTest(request_id++, 'test_page', [complex_node, simpleAnimal, anAnimal]);
    pageTreeRequestTest(request_id++, '', [complex_node, simpleAnimal, anAnimal]);
    pageTreeRequestTest(request_id++, 'https://example.com/path/to/resource?query=param#fragment', [complex_node, simpleAnimal, anAnimal]);
    pageTreeRequestTest(request_id++, '/lion/cub_1', [complex_node, simpleAnimal, anAnimal]);
    pageTreeRequestTest(request_id++, 'non_existent_page', []);
    pageTreeRequestTest(request_id++, 'https://example.com/path/to/non_existent_page', []);
    pageTreeRequestTest(request_id++, 'https://example.com/path/to/empty_page', []);
  });

  it('encode and decode getQueryNodesRequest', () => {
    function queryNodesRequestTest(request_id, req_label, resp_nodes) {
      const msg = new HTTP3TreeMessage().setRequestId(request_id);
      msg.encodeGetQueryNodesRequest(req_label);
      const reqBytes = decodeFromChunks(msg.requestChunks);
      const { value: decodedLabel } = decode_label(reqBytes, 0);
      expect(decodedLabel).toEqual(req_label);
      const respPayload = encode_vec_tree_node(resp_nodes);
      msg.responseChunks = encodeToChunks(respPayload, { signal: WWATP_SIGNAL.SIGNAL_WWATP_QUERY_NODES_RESPONSE, requestId: request_id });
      const out = msg.decodeGetQueryNodesResponse();
      expect(out.length).toBe(resp_nodes.length);
      out.forEach((n, i) => expect(n.equals(resp_nodes[i])).toBe(true));
    }

    let request_id = 6;
    const complex_node = createAnimalNode(
      'https://example.com/path/to/lion?query=param#fragment',
      'A complex animal',
      [{ type: 'txt', name: 'pup_1_dossier' }, { type: 'txt', name: 'pup_2_dossier' }],
      { versionNumber: 2, maxVersionSequence: 2 },
      ['pup_1', 'pup_2'],
      ['pup 1 dossier', 'pup 2 dossier'],
      'How to query complex animal',
      'Complex Animal QA sequence'
    );
    const simpleAnimal = createAnimalNode('Sponge', 'Bottom Feeder', [], { versionNumber: 1, maxVersionSequence: 1 }, [], [], '', '');
    const anAnimal = createAnimalNode(
      'Seal',
      'A marine mammal',
      [{ type: 'txt', name: 'dossier_1' }, { type: 'txt', name: 'dossier_2' }],
      { versionNumber: 1, maxVersionSequence: 1 },
      ['pup_1', 'pup_2'],
      ['pup 1 dossier', 'pup 2 dossier'],
      'How to query seal',
      'Seal QA sequence'
    );
    queryNodesRequestTest(request_id++, 'test_page', [anAnimal, complex_node]);
    queryNodesRequestTest(request_id++, 'https://example.com/path/to/lion?query=param#fragment', [anAnimal, complex_node, simpleAnimal]);
    queryNodesRequestTest(request_id++, '/Lion/Pup_1', [anAnimal, complex_node, simpleAnimal]);
    queryNodesRequestTest(request_id++, 'Sponge', [anAnimal, complex_node, simpleAnimal]);
    queryNodesRequestTest(request_id++, 'non_existent_query', []);
    queryNodesRequestTest(request_id++, 'https://example.com/path/to/non_existent_query', []);
    queryNodesRequestTest(request_id++, '', [complex_node]);
  });

  it('encode and decode openTransactionLayerRequest', () => {
    function openTransactionLayerTest(request_id, req_node, resp_bool) {
      const msg = new HTTP3TreeMessage().setRequestId(request_id);
      msg.encodeOpenTransactionLayerRequest(req_node);
      const reqBytes = decodeFromChunks(msg.requestChunks);
      const { value: maybeNode } = decode_maybe_tree_node(reqBytes, 0);
      expect(maybeNode.isJust()).toBe(true);
      expect(maybeNode.value.equals(req_node)).toBe(true);
      msg.responseChunks = encodeToChunks(new Uint8Array([resp_bool ? 1 : 0]), { signal: WWATP_SIGNAL.SIGNAL_WWATP_OPEN_TRANSACTION_LAYER_RESPONSE, requestId: request_id });
      expect(msg.decodeOpenTransactionLayerResponse()).toBe(resp_bool);
    }

    let request_id = 6;
    const complex_node = createAnimalNode(
      'https://example.com/path/to/lion?query=param#fragment',
      'A complex animal',
      [{ type: 'txt', name: 'pup_1_dossier' }, { type: 'txt', name: 'pup_2_dossier' }],
      { versionNumber: 2, maxVersionSequence: 2 },
      ['pup_1', 'pup_2'],
      ['pup 1 dossier', 'pup 2 dossier'],
      'How to query complex animal',
      'Complex Animal QA sequence'
    );
    const simpleAnimal = createAnimalNode('Sponge', 'Bottom Feeder', [], { versionNumber: 1, maxVersionSequence: 1 }, [], [], '', '');
    const anAnimal = createAnimalNode(
      'Seal',
      'A marine mammal',
      [{ type: 'txt', name: 'dossier_1' }, { type: 'txt', name: 'dossier_2' }],
      { versionNumber: 1, maxVersionSequence: 1 },
      ['pup_1', 'pup_2'],
      ['pup 1 dossier', 'pup 2 dossier'],
      'How to query seal',
      'Seal QA sequence'
    );

    openTransactionLayerTest(request_id++, complex_node, true);
    openTransactionLayerTest(request_id++, simpleAnimal, true);
    openTransactionLayerTest(request_id++, anAnimal, false);
  });

  it('encode and decode closeTransactionLayersRequest', () => {
    function closeTransactionLayersTest(request_id, _req_bool, resp_bool) {
      const msg = new HTTP3TreeMessage().setRequestId(request_id);
      msg.encodeCloseTransactionLayersRequest();
      const reqBytes = decodeFromChunks(msg.requestChunks);
      expect(reqBytes.byteLength).toBe(0);
      msg.responseChunks = encodeToChunks(new Uint8Array([resp_bool ? 1 : 0]), { signal: WWATP_SIGNAL.SIGNAL_WWATP_CLOSE_TRANSACTION_LAYERS_RESPONSE, requestId: request_id });
      expect(msg.decodeCloseTransactionLayersResponse()).toBe(resp_bool);
    }

    let request_id = 6;
    closeTransactionLayersTest(request_id++, true, true);
    closeTransactionLayersTest(request_id++, true, false);
  });

  it('encode and decode applyTransactionRequest', () => {
    function applyTransactionTest(request_id, req_tx, resp_bool) {
      const msg = new HTTP3TreeMessage().setRequestId(request_id);
      msg.encodeApplyTransactionRequest(req_tx);
      const reqBytes = decodeFromChunks(msg.requestChunks);
      const { value: decodedTx } = decode_transaction(reqBytes, 0);
      // compare lengths only for sanity
      expect(decodedTx.length).toBe(req_tx.length);
      msg.responseChunks = encodeToChunks(new Uint8Array([resp_bool ? 1 : 0]), { signal: WWATP_SIGNAL.SIGNAL_WWATP_APPLY_TRANSACTION_RESPONSE, requestId: request_id });
      expect(msg.decodeApplyTransactionResponse()).toBe(resp_bool);
    }

    let request_id = 6;
    const complex_node = createAnimalNode(
      'https://example.com/path/to/lion?query=param#fragment',
      'A complex animal',
      [{ type: 'txt', name: 'pup_1_dossier' }, { type: 'txt', name: 'pup_2_dossier' }],
      { versionNumber: 2, maxVersionSequence: 2 },
      ['pup_1', 'pup_2'],
      ['pup 1 dossier', 'pup 2 dossier'],
      'How to query complex animal',
      'Complex Animal QA sequence'
    );
    const simpleAnimal = createAnimalNode('Sponge', 'Bottom Feeder', [], { versionNumber: 1, maxVersionSequence: 1 }, [], [], '', '');
  const emptiest_version = [Nothing, ['', Nothing]];
  const emptier_version = [Nothing, ['/lion/cub_1', Nothing]];
    const empty_version = [Just(0), ['/lion/cub_1', Nothing]];

    const simple_version = [Just(13), [simpleAnimal.getLabelRule(), Just(simpleAnimal)]];
    const complex_version = [Just(12233), [complex_node.getLabelRule(), Just(complex_node)]];

    const empty_sub_transaction = [emptiest_version, []];
    const simple_sub_transaction = [empty_version, [emptiest_version]];
    const basic_sub_transaction = [simple_version, [emptier_version, simple_version, empty_version, simple_version]];
    const complex_sub_transaction = [complex_version, [emptier_version, simple_version, empty_version, simple_version, complex_version]];

    const empty_transaction = [];
    const simple_transaction = [empty_sub_transaction];
    const basic_transaction = [basic_sub_transaction, simple_sub_transaction];
    const complex_transaction = [empty_sub_transaction, simple_sub_transaction, basic_sub_transaction, complex_sub_transaction];

    applyTransactionTest(request_id++, empty_transaction, true);
    applyTransactionTest(request_id++, simple_transaction, true);
    applyTransactionTest(request_id++, basic_transaction, true);
    applyTransactionTest(request_id++, complex_transaction, true);
    applyTransactionTest(request_id++, [], false);
    applyTransactionTest(request_id++, empty_transaction, false);
    applyTransactionTest(request_id++, simple_transaction, false);
  });

  it('encode and decode getFullTreeRequest', () => {
    function fullTreeRequestTest(request_id, _req_bool, resp_nodes) {
      const msg = new HTTP3TreeMessage().setRequestId(request_id);
      msg.encodeGetFullTreeRequest();
      const reqBytes = decodeFromChunks(msg.requestChunks);
      expect(reqBytes.byteLength).toBe(0);
      msg.responseChunks = encodeToChunks(encode_vec_tree_node(resp_nodes), { signal: WWATP_SIGNAL.SIGNAL_WWATP_GET_FULL_TREE_RESPONSE, requestId: request_id });
      const out = msg.decodeGetFullTreeResponse();
      expect(out.length).toBe(resp_nodes.length);
      out.forEach((n, i) => expect(n.equals(resp_nodes[i])).toBe(true));
    }

    let request_id = 6;
    const complex_node = createAnimalNode(
      'https://example.com/path/to/lion?query=param#fragment',
      'A complex animal',
      [{ type: 'txt', name: 'pup_1_dossier' }, { type: 'txt', name: 'pup_2_dossier' }],
      { versionNumber: 2, maxVersionSequence: 2 },
      ['pup_1', 'pup_2'],
      ['pup 1 dossier', 'pup 2 dossier'],
      'How to query complex animal',
      'Complex Animal QA sequence'
    );
    const simpleAnimal = createAnimalNode('Sponge', 'Bottom Feeder', [], { versionNumber: 1, maxVersionSequence: 1 }, [], [], '', '');
    const anAnimal = createAnimalNode(
      'Seal',
      'A marine mammal',
      [{ type: 'txt', name: 'dossier_1' }, { type: 'txt', name: 'dossier_2' }],
      { versionNumber: 1, maxVersionSequence: 1 },
      ['pup_1', 'pup_2'],
      ['pup 1 dossier', 'pup 2 dossier'],
      'How to query seal',
      'Seal QA sequence'
    );
    fullTreeRequestTest(request_id++, true, [anAnimal, complex_node, simpleAnimal]);
    fullTreeRequestTest(request_id++, true, [complex_node]);
    fullTreeRequestTest(request_id++, true, [anAnimal]);
    fullTreeRequestTest(request_id++, true, []);
  });

  it('encode and decode registerNodeListenerRequest', () => {
    function registerNodeListenerTest(request_id, tuple, resp_bool) {
      const [listener, label, childNotify] = tuple;
      const msg = new HTTP3TreeMessage().setRequestId(request_id);
      msg.encodeRegisterNodeListenerRequest(listener, label, childNotify);
      const reqBytes = decodeFromChunks(msg.requestChunks);
      let o = 0; const a = decode_label(reqBytes, o); o += a.read; const b = decode_label(reqBytes, o); o += b.read;
      const c = reqBytes[o] !== 0;
      expect(a.value).toEqual(listener);
      expect(b.value).toEqual(label);
      expect(c).toBe(childNotify);
      msg.responseChunks = encodeToChunks(new Uint8Array([resp_bool ? 1 : 0]), { signal: WWATP_SIGNAL.SIGNAL_WWATP_REGISTER_LISTENER_RESPONSE, requestId: request_id });
      expect(msg.decodeRegisterNodeListenerResponse()).toBe(resp_bool);
    }

    let request_id = 6;
    registerNodeListenerTest(request_id++, ['listener1', 'test_label', true], true);
    registerNodeListenerTest(request_id++, ['listener2', '', false], true);
    registerNodeListenerTest(request_id++, ['', 'test_label', true], false);
    registerNodeListenerTest(request_id++, ['listener3', 'https://example.com/path/to/resource?query=param#fragment', true], true);
  });

  it('encode and decode deregisterNodeListenerRequest', () => {
    function deregisterNodeListenerTest(request_id, pair, resp_bool) {
      const [listener, label] = pair;
      const msg = new HTTP3TreeMessage().setRequestId(request_id);
      msg.encodeDeregisterNodeListenerRequest(listener, label);
      const reqBytes = decodeFromChunks(msg.requestChunks);
      let o = 0; const a = decode_label(reqBytes, o); o += a.read; const b = decode_label(reqBytes, o); o += b.read;
      expect(a.value).toEqual(listener);
      expect(b.value).toEqual(label);
      msg.responseChunks = encodeToChunks(new Uint8Array([resp_bool ? 1 : 0]), { signal: WWATP_SIGNAL.SIGNAL_WWATP_DEREGISTER_LISTENER_RESPONSE, requestId: request_id });
      expect(msg.decodeDeregisterNodeListenerResponse()).toBe(resp_bool);
    }

    let request_id = 6;
    deregisterNodeListenerTest(request_id++, ['listener1', 'test_label'], true);
    deregisterNodeListenerTest(request_id++, ['listener2', ''], true);
    deregisterNodeListenerTest(request_id++, ['', 'test_label'], false);
    deregisterNodeListenerTest(request_id++, ['listener3', 'https://example.com/path/to/resource?query=param#fragment'], true);
  });

  it('encode and decode notifyListenersRequest', () => {
    function notifyListenersTest(request_id, pair, resp_bool) {
      const [label, maybeNode] = pair; // in C++ this is listenerName? Here adapt to label for encode
      const msg = new HTTP3TreeMessage().setRequestId(request_id);
      msg.encodeNotifyListenersRequest(label, maybeNode);
      const reqBytes = decodeFromChunks(msg.requestChunks);
      let o = 0; const a = decode_label(reqBytes, o); o += a.read; const b = decode_maybe_tree_node(reqBytes, o);
      expect(a.value).toEqual(label);
      if (maybeNode.isJust()) {
        expect(b.value.isJust()).toBe(true);
        expect(b.value.value.equals(maybeNode.value)).toBe(true);
      } else {
        expect(b.value.isNothing()).toBe(true);
      }
      msg.responseChunks = encodeToChunks(new Uint8Array([resp_bool ? 1 : 0]), { signal: WWATP_SIGNAL.SIGNAL_WWATP_NOTIFY_LISTENERS_RESPONSE, requestId: request_id });
      expect(msg.decodeNotifyListenersResponse()).toBe(resp_bool);
    }

    let request_id = 6;
    const complex_node = createAnimalNode(
      'https://example.com/path/to/lion?query=param#fragment',
      'A complex animal',
      [{ type: 'txt', name: 'pup_1_dossier' }, { type: 'txt', name: 'pup_2_dossier' }],
      { versionNumber: 2, maxVersionSequence: 2 },
      ['pup_1', 'pup_2'],
      ['pup 1 dossier', 'pup 2 dossier'],
      'How to query complex animal',
      'Complex Animal QA sequence'
    );
    const simpleAnimal = createAnimalNode('Sponge', 'Bottom Feeder', [], { versionNumber: 1, maxVersionSequence: 1 }, [], [], '', '');
    const anAnimal = createAnimalNode(
      'Seal',
      'A marine mammal',
      [{ type: 'txt', name: 'dossier_1' }, { type: 'txt', name: 'dossier_2' }],
      { versionNumber: 1, maxVersionSequence: 1 },
      ['pup_1', 'pup_2'],
      ['pup 1 dossier', 'pup 2 dossier'],
      'How to query seal',
      'Seal QA sequence'
    );
    notifyListenersTest(request_id++, ['listener1', Just(anAnimal)], true);
    notifyListenersTest(request_id++, ['listener2', Just(complex_node)], true);
    notifyListenersTest(request_id++, ['listener3', Nothing], false);
    notifyListenersTest(request_id++, ['', Just(simpleAnimal)], true);
    notifyListenersTest(request_id++, ['', Nothing], true);
  });

  it('encode and decode processNotificationRequest', () => {
    function processNotificationTest(request_id, _req_bool, resp_bool) {
      const msg = new HTTP3TreeMessage().setRequestId(request_id);
      msg.encodeProcessNotificationRequest();
      const reqBytes = decodeFromChunks(msg.requestChunks);
      expect(reqBytes.byteLength).toBe(0);
      msg.responseChunks = encodeToChunks(new Uint8Array([resp_bool ? 1 : 0]), { signal: WWATP_SIGNAL.SIGNAL_WWATP_PROCESS_NOTIFICATION_RESPONSE, requestId: request_id });
      expect(msg.decodeProcessNotificationResponse()).toBe(resp_bool);
    }

    let request_id = 6;
    processNotificationTest(request_id++, true, true);
  });

  it('encode and decode getJournalRequest', () => {
    function getJournalRequestTest(request_id, lastNotif, respNotifs) {
      const msg = new HTTP3TreeMessage().setRequestId(request_id);
      msg.encodeGetJournalRequest(lastNotif);
      const reqBytes = decodeFromChunks(msg.requestChunks);
      // request payload should have empty label and Nothing per spec
      const { value: decodedSN } = decode_sequential_notification(reqBytes, 0);
      expect(decodedSN.notification.labelRule).toEqual('');
      expect(decodedSN.notification.maybeNode.isNothing()).toBe(true);
      const respPayload = encode_vec_sequential_notification(respNotifs);
      msg.responseChunks = encodeToChunks(respPayload, { signal: WWATP_SIGNAL.SIGNAL_WWATP_GET_JOURNAL_RESPONSE, requestId: request_id });
      const out = msg.decodeGetJournalResponse();
      expect(out.length).toBe(respNotifs.length);
      // spot check equality (signalCount and label)
      out.forEach((sn, i) => {
        expect(sn.signalCount).toBe(respNotifs[i].signalCount);
        expect(sn.notification.labelRule).toBe(respNotifs[i].notification.labelRule);
      });
    }

    let request_id = 6;
    const empty = { signalCount: 0, notification: { labelRule: '', maybeNode: Nothing } };
    const single = { signalCount: 1, notification: { labelRule: '', maybeNode: Nothing } };
    const complex_node = createAnimalNode(
      'https://example.com/path/to/lion?query=param#fragment',
      'A complex animal',
      [{ type: 'txt', name: 'pup_1_dossier' }, { type: 'txt', name: 'pup_2_dossier' }],
      { versionNumber: 2, maxVersionSequence: 2 },
      ['pup_1', 'pup_2'],
      ['pup 1 dossier', 'pup 2 dossier'],
      'How to query complex animal',
      'Complex Animal QA sequence'
    );
    const simpleAnimal = createAnimalNode('Sponge', 'Bottom Feeder', [], { versionNumber: 1, maxVersionSequence: 1 }, [], [], '', '');
    const complex_vector = [
      { signalCount: 10, notification: { labelRule: '/lion/pup_1', maybeNode: Nothing } },
      { signalCount: 11, notification: { labelRule: complex_node.getLabelRule(), maybeNode: Just(complex_node) } },
      { signalCount: 12, notification: { labelRule: '/lion/pup_2', maybeNode: Nothing } },
      { signalCount: 13, notification: { labelRule: complex_node.getLabelRule(), maybeNode: Just(simpleAnimal) } },
      { signalCount: 14, notification: { labelRule: complex_node.getLabelRule(), maybeNode: Just(complex_node) } },
      { signalCount: 12, notification: { labelRule: '/lion/pup_2', maybeNode: Nothing } },
    ];

    getJournalRequestTest(request_id++, empty, []);
    getJournalRequestTest(request_id++, single, [single]);
    getJournalRequestTest(request_id++, single, complex_vector);
  });
});
