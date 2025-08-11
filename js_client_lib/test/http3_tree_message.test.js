import { describe, it, expect } from 'vitest';
import HTTP3TreeMessage from '../interface/http3_tree_message.js';
import {
  WWATP_SIGNAL,
  encode_label,
  decode_label,
  encode_vec_tree_node,
  decode_vec_tree_node,
  encode_maybe_tree_node,
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
  request_signal,
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
  it.todo('encode and decode deleteNodeRequest');
  it.todo('encode and decode getPageTreeRequest');
  it.todo('encode and decode getQueryNodesRequest');
  it.todo('encode and decode openTransactionLayerRequest');
  it.todo('encode and decode closeTransactionLayersRequest');
  it.todo('encode and decode applyTransactionRequest');
  it.todo('encode and decode getFullTreeRequest');
  it.todo('encode and decode registerNodeListenerRequest');
  it.todo('encode and decode deregisterNodeListenerRequest');
  it.todo('encode and decode notifyListenersRequest');
  it.todo('encode and decode processNotificationRequest');
  it.todo('encode and decode getJournalRequest');
});
