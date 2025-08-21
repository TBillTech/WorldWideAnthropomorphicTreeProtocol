import { describe, it, expect } from 'vitest';
import { NodeWebTransportMock, Request, SimpleBackend, Http3ClientBackendUpdater } from '../../index.js';
import { BackendTestbed } from '../backend_testbed/backend_testbed.js';
import { createWWATPHandler } from './server_mock.js';

// System-level test mirroring the mock transport suite but using NodeWebTransportMock

describe('System (node webtransport mock) – end-to-end', () => {
  it('add animals + notes + test backend logically', async () => {
    const serverBackend = new SimpleBackend();
    const comm = new NodeWebTransportMock('mock://system');
    comm.setMockHandler(createWWATPHandler(serverBackend));
    await comm.connect();

    const updater = new Http3ClientBackendUpdater('sysNW', 'local', 0);
    const localA = new SimpleBackend();
    const req = new Request({ path: '/api/wwatp' });
    const clientA = updater.addBackend(localA, true, req, 120);

    const maintain = async () => { await updater.maintainRequestHandlers(comm, 0); };

    const tbServer = new BackendTestbed(serverBackend);
    tbServer.addAnimalsToBackend();
    tbServer.addNotesPageTree();

  clientA.requestFullTreeSync();
  await maintain();
  const vec = clientA.getFullTree();
    expect(Array.isArray(vec)).toBe(true);

    const tbClientA = new BackendTestbed(localA, { shouldTestChanges: true });
    tbClientA.testBackendLogically('');

    await comm.close();
  });
});
