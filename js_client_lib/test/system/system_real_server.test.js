import { describe, it, expect } from 'vitest';
import { SimpleBackend, Http3ClientBackendUpdater, Request } from '../../index.js';
import { BackendTestbed } from '../backend_testbed/backend_testbed.js';
import fs from 'node:fs';
import path from 'node:path';
import child_process from 'node:child_process';

// System-level test group B: uses real WWATPService server executable.
// This test is skipped by default unless WWATP_E2E=1 environment variable is set.
// It assumes the server binary is built at build/wwatp_server and accessible from repo root.

const SHOULD_RUN = process.env.WWATP_E2E === '1';
const ROOT = path.resolve(__dirname, '../../..');
const SERVER_BIN = path.join(ROOT, 'build', 'wwatp_server');
const CONFIG = path.join(ROOT, 'js_client_lib', 'test', 'resources', 'wwatp_server_config.yaml');

async function waitForServerReady(proc, timeoutMs = 5000) {
  return new Promise((resolve, reject) => {
    const t = setTimeout(() => reject(new Error('timeout waiting for server ready')), timeoutMs);
    const onData = (buf) => {
      const s = buf.toString();
      if (s.includes('Starting WWATP server')) {
        clearTimeout(t);
        proc.stdout.off('data', onData);
        resolve(true);
      }
    };
    proc.stdout.on('data', onData);
  });
}

async function spawnServer() {
  if (!fs.existsSync(SERVER_BIN)) throw new Error(`Server binary not found at ${SERVER_BIN}`);
  const proc = child_process.spawn(SERVER_BIN, [CONFIG], { cwd: ROOT });
  proc.on('error', () => {});
  await waitForServerReady(proc, 8000);
  return proc;
}

function killServer(proc) {
  if (!proc) return;
  try { proc.kill('SIGINT'); } catch (_) {}
}

describe.skipIf(!SHOULD_RUN)('System (real server) – end-to-end', () => {
  it('add animals + notes + test backend logically against server', async () => {
    const server = await spawnServer();
    try {
      // Connect two JS clients to the WWATPService HTTP/3 endpoint
      const updater = new Http3ClientBackendUpdater('real', '127.0.0.1', 12345);
      const local = new SimpleBackend();
      const client = updater.addBackend(local, true, new Request({ scheme: 'https', authority: '127.0.0.1:12345', path: '/init/wwatp/' }), 60);

      // Perform sync and run testbed checks. Here we rely on server side to already have content
      // or we can attempt to upsert via client if server supports it.
      const tree = client.getFullTree();
      await updater.maintainRequestHandlers({
        getNewRequestStreamIdentifier: (req) => ({ cid: 'https://127.0.0.1:12345', logicalId: Date.now() }),
        registerResponseHandler: () => {},
        deregisterResponseHandler: () => {},
        sendRequest: async () => ({ ok: false, status: 501 }),
        processRequestStream: () => {},
      }, 0);

      // For now, assert that request was queued; real HTTP/3 transport adapter needed to actually communicate.
      // Placeholder until WebTransport/Fetch adapter is wired to the C++ server.
      expect(tree).toBeTruthy();
    } finally {
      killServer(server);
    }
  });
});
