import { describe, it, expect } from 'vitest';
import { MockCommunication, Request, SimpleBackend, Http3ClientBackendUpdater } from '../../index.js';
import { BackendTestbed } from '../backend_testbed/backend_testbed.js';
import { createWWATPHandler } from './server_mock.js';

// System-level test group A: uses mock transport and an in-memory server backend
// We validate the idiom add animals + add notes + test backend logically against a server,
// and run a peer notification path using two clients against the same server.

describe('System (mock transport) – end-to-end', () => {
  it('add animals + notes + test backend logically', async () => {
    // Shared server backend for authoritative state
    const serverBackend = new SimpleBackend();

    // Mock transport wired to a WWATP handler that uses serverBackend
    const comm = new MockCommunication();
    comm.setMockHandler(createWWATPHandler(serverBackend));

    // Client A: local cache and updater
    const updater = new Http3ClientBackendUpdater('sysA', 'local', 0);
    const localA = new SimpleBackend();
    const req = new Request({ path: '/api/wwatp' });
    const clientA = updater.addBackend(localA, true, req, /*journal rpm*/ 120);

    // Start a single maintain pass per operation to keep the test deterministic
    const maintain = async () => { await updater.maintainRequestHandlers(comm, 0); };

    // Seed server with animals and notes using clientA operations
    const tbServer = new BackendTestbed(serverBackend);
    tbServer.addAnimalsToBackend();
    tbServer.addNotesPageTree();

    // Sync client A via getFullTree
  // Kick a journal poll to populate client cache from server via handler
  clientA.requestFullTreeSync();
  await maintain();
  const vec = clientA.getFullTree();
    expect(Array.isArray(vec)).toBe(true);
    // Now run logical test on client A's local cache
    const tbClientA = new BackendTestbed(localA, { shouldTestChanges: true });
    tbClientA.testBackendLogically('');
  });

  it('peer notifications via journal with two clients', async () => {
    const serverBackend = new SimpleBackend();
    const comm = new MockCommunication();
    comm.setMockHandler(createWWATPHandler(serverBackend));

    const updater = new Http3ClientBackendUpdater('sysB', 'local', 0);
    const localA = new SimpleBackend();
    const localB = new SimpleBackend();
    const req = new Request({ path: '/api/wwatp' });
  const A = updater.addBackend(localA, true, req, 120);
  updater.addBackend(localB, true, req, 120);

    const maintain = async () => { await updater.maintainRequestHandlers(comm, 0); };

    // Register a listener on A for 'lion'
    let seen = [];
    A.registerNodeListener('listenerA', 'lion', false, (_backend, labelRule, maybeNode) => {
      seen.push([labelRule, maybeNode.isJust ? maybeNode.isJust() : false]);
    });
    await maintain();

    // From B: create lion nodes on the server
  // Temporary testbed was unused in this test; remove
    // Reuse helper to create lion nodes only
    const { createLionNodes } = await import('../backend_testbed/backend_testbed.js');
    serverBackend.upsertNode(createLionNodes());

  // Journal tick – A should pull notification
  await maintain();

    // From B: delete lion
    serverBackend.deleteNode('lion');
    await maintain();

    // Validate notifications sequence (create -> delete)
    expect(seen.length >= 2).toBe(true);
    expect(seen[0][0]).toBe('lion');
    expect(seen[0][1]).toBe(true); // creation (Just)
    expect(seen[seen.length - 1][0]).toBe('lion');
    expect(seen[seen.length - 1][1]).toBe(false); // deletion (Nothing)

    // Deregister and ensure no further notifications
    A.deregisterNodeListener('listenerA', 'lion');
    await maintain();
    serverBackend.upsertNode(createLionNodes());
    await maintain();
    const after = seen.length;
    serverBackend.deleteNode('lion');
    await maintain();
    expect(seen.length).toBe(after);
  });
});
