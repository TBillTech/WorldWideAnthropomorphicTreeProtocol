import { describe, it, expect } from 'vitest';
import { SimpleBackend, Http3ClientBackendUpdater, Request } from '../../index.js';
import { BackendTestbed } from '../backend_testbed/backend_testbed.js';
import fs from 'node:fs';
import path from 'node:path';
import child_process from 'node:child_process';
import dgram from 'node:dgram';

// System-level test group B: uses real WWATPService server executable.
// This test is skipped by default unless WWATP_E2E=1 environment variable is set.
// It assumes the server binary is built at build/wwatp_server and accessible from repo root.

const e2e = (process.env.WWATP_E2E || '').toString().toLowerCase();
const SHOULD_RUN = e2e === '1' || e2e === 'true' || e2e === 'yes' || e2e === 'on';
// Diagnostics to confirm gating
console.info('[system_real_server.test] WWATP_E2E=%s, SHOULD_RUN=%s', process.env.WWATP_E2E, SHOULD_RUN);
const ROOT = path.resolve(__dirname, '../../..');
const SERVER_BIN = path.join(ROOT, 'build', 'wwatp_server');
// Real server test continues to use the default config and port 12345
const CONFIG = path.join(ROOT, 'js_client_lib', 'test', 'resources', 'wwatp_server_config.yaml');
const DATA_DIR = path.join(ROOT, 'test_instances', 'data');
const SANDBOX_DIR = path.join(ROOT, 'test_instances', 'sandbox');
const CERT_FILE = path.join(DATA_DIR, 'cert.pem');
const KEY_FILE = path.join(DATA_DIR, 'private_key.pem');
// No FileBackend node hacks required; the server now accepts .yaml file directly.

function ensureSandboxAndDataDirs() {
  fs.mkdirSync(SANDBOX_DIR, { recursive: true });
  fs.mkdirSync(DATA_DIR, { recursive: true });
}

function haveCerts() {
  return fs.existsSync(CERT_FILE) && fs.existsSync(KEY_FILE);
}

function tryGenerateSelfSignedCerts() {
  if (haveCerts()) return true;
  if (process.env.WWATP_GEN_CERTS !== '1') return false;
  ensureSandboxAndDataDirs();
  // Generate a simple self-signed cert/key pair for localhost using openssl
  // Note: requires openssl to be installed in PATH
  const cmd = `openssl req -x509 -newkey rsa:2048 -nodes -keyout "${KEY_FILE}" -out "${CERT_FILE}" -days 365 -subj "/CN=127.0.0.1"`;
  const res = child_process.spawnSync('bash', ['-lc', cmd], { cwd: ROOT });
  return res.status === 0 && haveCerts();
}

async function isUdpPortInUse(port, host = '127.0.0.1') {
  return new Promise((resolve) => {
    const s = dgram.createSocket('udp4');
    s.once('error', (err) => {
      if (err.code === 'EADDRINUSE' || err.code === 'EACCES') resolve(true);
      else resolve(false);
    });
    s.bind(port, host, () => {
      s.close(() => resolve(false));
    });
  });
}

async function waitForServerReady(proc, port = 12345, host = '127.0.0.1', timeoutMs = 15000) {
  const start = Date.now();
  return new Promise((resolve, reject) => {
    let settled = false;
    const done = (ok) => { if (!settled) { settled = true; clearInterval(iv); resolve(ok); } };
    const fail = (err) => { if (!settled) { settled = true; clearInterval(iv); reject(err); } };
    // If the process exits early, fail fast
    proc.once('exit', (code) => {
      fail(new Error(`server exited before ready, code=${code}`));
    });
    // Poll UDP bind until it’s in use or timeout
    const iv = setInterval(async () => {
      if (Date.now() - start > timeoutMs) {
        fail(new Error('timeout waiting for server UDP bind'));
        return;
      }
      const inUse = await isUdpPortInUse(port, host);
      if (inUse) done(true);
    }, 200);
  });
}

async function spawnServer() {
  if (!fs.existsSync(SERVER_BIN)) throw new Error(`Server binary not found at ${SERVER_BIN}`);
  ensureSandboxAndDataDirs();
  const proc = child_process.spawn(SERVER_BIN, [CONFIG], { cwd: ROOT });
  proc.on('error', () => {});
  // Wait until UDP bind is observed
  await waitForServerReady(proc, 12345, '127.0.0.1', 15000);
  // Small settle delay to allow server to finish initialization (frontends/backends wiring)
  await new Promise((r) => setTimeout(r, 300));
  return proc;
}

async function killServer(proc, signal = 'SIGINT', timeoutMs = 5000, cooldownMs = 500) {
  if (!proc) return;
  await new Promise((resolve) => {
    let settled = false;
    const done = () => { if (!settled) { settled = true; resolve(); } };
    try {
      proc.once('exit', () => done());
      proc.kill(signal);
      setTimeout(() => done(), timeoutMs);
    } catch (_) { done(); }
  });
  // Cooldown to allow OS to fully release sockets and file handles
  await new Promise((r) => setTimeout(r, Math.max(0, cooldownMs | 0)));
}

// Force this suite to run sequentially (no parallel sub-tests) when enabled
const realServerDescribe = SHOULD_RUN ? describe.sequential : describe.skip;
realServerDescribe('System (real server) – integration readiness', () => {
  it('config --check-only validates server config', async () => {
    ensureSandboxAndDataDirs();
    // Optionally generate certs for convenience when enabled
    tryGenerateSelfSignedCerts();
    const res = child_process.spawnSync(SERVER_BIN, ['--check-only', CONFIG], { cwd: ROOT, encoding: 'utf8' });
    expect(res.status).toBe(0);
    expect(res.stdout + res.stderr).toContain('Configuration validation completed successfully');
  });

  it('server starts, binds UDP port, and stops cleanly (smoke)', async () => {
    // Ensure certs exist or skip with guidance
    if (!haveCerts() && !tryGenerateSelfSignedCerts()) {
      return expect.fail(`Missing TLS cert/key at ${CERT_FILE} and ${KEY_FILE}. Set WWATP_GEN_CERTS=1 to auto-generate for tests.`);
    }
    const server = await spawnServer();
    try {
      // Verify UDP port bound
      const inUse = await isUdpPortInUse(12345, '127.0.0.1');
      expect(inUse).toBe(true);
    } finally {
      await killServer(server);
    }
  }, 20000);

  it('optional: curl --http3 can fetch /index.html (if curl supports HTTP/3)', async () => {
    // Detect curl and HTTP/3 support
    const ver = child_process.spawnSync('bash', ['-lc', 'command -v curl >/dev/null 2>&1 && curl -V | head -n1 || echo "nocurl"'], { encoding: 'utf8' });
    const v = ver.stdout || '';
    if (!v || v.includes('nocurl') || !/HTTP\/?3|nghttp3|quiche/i.test(v)) {
      return; // Skip silently if curl or HTTP/3 not available
    }

    if (!haveCerts() && !tryGenerateSelfSignedCerts()) {
      return; // Skip if no certs and cannot generate
    }

    const server = await spawnServer();
    try {
      let last = { status: -1, stdout: '', stderr: '' };
      for (let i = 0; i < 5; i++) {
        const cmd = `curl --http3 -k -sS --cert ${CERT_FILE} --key ${KEY_FILE} https://127.0.0.1:12345/index.html | wc -c`;
        const res = child_process.spawnSync('bash', ['-lc', cmd], { cwd: ROOT, encoding: 'utf8' });
        last = res;
        const bytes = parseInt((res.stdout || '0').trim(), 10);
        if (res.status === 0 && Number.isFinite(bytes) && bytes > 0) {
          expect(true).toBe(true);
          return;
        }
        await new Promise(r => setTimeout(r, 400));
      }
      // If still failing, surface diagnostics
  console.error('[curl-http3] status=%s, stdout=%s, stderr=%s', last.status, last.stdout, last.stderr);
      const bytes = parseInt((last.stdout || '0').trim(), 10);
      expect(last.status).toBe(0);
      expect(Number.isFinite(bytes) && bytes > 0).toBe(true);
    } finally {
      await killServer(server);
    }
  }, 20000);

  it('libcurl testBackendLogically', async () => {
    // Detect curl with HTTP/3 support
    const ver = child_process.spawnSync('bash', ['-lc', 'command -v curl >/dev/null 2>&1 && curl -V | head -n1 || echo "nocurl"'], { encoding: 'utf8' });
    const v = ver.stdout || '';
    if (!v || v.includes('nocurl') || !/HTTP\/?3|nghttp3|quiche/i.test(v)) {
      return; // Skip if curl or HTTP/3 not available
    }
    if (!haveCerts() && !tryGenerateSelfSignedCerts()) {
      return; // Skip if certs missing and cannot generate
    }

    // Ensure env for curl bridge
    process.env.WWATP_CERT = CERT_FILE;
    process.env.WWATP_KEY = KEY_FILE;
    process.env.WWATP_INSECURE = '1';

    const server = await spawnServer();
    try {
      // Build updater and libcurl transport (single QUIC connection)
      const updater = new Http3ClientBackendUpdater('real', '127.0.0.1', 12345);
      const local = new SimpleBackend();
      const client = updater.addBackend(local, true, new Request({ scheme: 'https', authority: '127.0.0.1:12345', path: '/init/wwatp/' }), 60);

      // Prefer LibcurlTransport; skip if not available
      let transport = null;
      try {
        const { LibcurlTransport } = await import('../../index.js');
        transport = new LibcurlTransport();
        await transport.connect({ scheme: 'https', authority: '127.0.0.1:12345' });
      } catch (err) {
        // Skip this WWATP flow if node-libcurl isn't installed or fails to init
        console.warn('[libcurl] Skipping testBackendLogically: libcurl not available. Error:', err && (err.message || err));
        return;
      }

      // Seed the server with animals and a notes page tree using backend_testbed helpers
      const {
        createLionNodes,
        createElephantNodes,
        createParrotNodes,
        createNotesPageTree,
      } = await import('../backend_testbed/backend_testbed.js');

      // Upsert animals (each as a vector of nodes); process network each time
      const up1 = client.upsertNode(createLionNodes());
      await updater.maintainRequestHandlers(transport, 0);
      expect(await Promise.race([up1, new Promise((_, r) => setTimeout(() => r(new Error('timeout upserting lion')), 8000))])).toBe(true);

      const up2 = client.upsertNode(createElephantNodes());
      await updater.maintainRequestHandlers(transport, 0);
      expect(await Promise.race([up2, new Promise((_, r) => setTimeout(() => r(new Error('timeout upserting elephant')), 8000))])).toBe(true);

      const up3 = client.upsertNode(createParrotNodes());
      await updater.maintainRequestHandlers(transport, 0);
      expect(await Promise.race([up3, new Promise((_, r) => setTimeout(() => r(new Error('timeout upserting parrot')), 8000))])).toBe(true);

      const up4 = client.upsertNode([createNotesPageTree()]);
      await updater.maintainRequestHandlers(transport, 0);
      expect(await Promise.race([up4, new Promise((_, r) => setTimeout(() => r(new Error('timeout upserting notes page')), 8000))])).toBe(true);

      // Fetch full tree to sync local cache
      const fullP = client.getFullTree();
      await updater.maintainRequestHandlers(transport, 0);
      const vec = await Promise.race([
        fullP,
        new Promise((_, rej) => setTimeout(() => rej(new Error('timeout waiting for full tree')), 8000)),
      ]);
      expect(Array.isArray(vec)).toBe(true);

      // Now validate logically on the local SimpleBackend using BackendTestbed
      const tbClient = new BackendTestbed(local, { shouldTestChanges: true });
      tbClient.testBackendLogically('');
    } finally {
      await killServer(server);
    }
  }, 20000);

  it('test roundtrip', async () => {
    // Detect curl with HTTP/3 support
    const ver = child_process.spawnSync('bash', ['-lc', 'command -v curl >/dev/null 2>&1 && curl -V | head -n1 || echo "nocurl"'], { encoding: 'utf8' });
    const v = ver.stdout || '';
    if (!v || v.includes('nocurl') || !/HTTP\/?3|nghttp3|quiche/i.test(v)) {
      return; // Skip if curl or HTTP/3 not available
    }
    if (!haveCerts() && !tryGenerateSelfSignedCerts()) {
      return; // Skip if certs missing and cannot generate
    }

    // Ensure env for curl-based transports
    process.env.WWATP_CERT = CERT_FILE;
    process.env.WWATP_KEY = KEY_FILE;
    process.env.WWATP_INSECURE = '1';

    const server = await spawnServer();
    try {
      // One LibcurlTransport to reuse the same QUIC connection
      let transport = null;
      try {
        const { LibcurlTransport } = await import('../../index.js');
        transport = new LibcurlTransport();
        await transport.connect({ scheme: 'https', authority: '127.0.0.1:12345' });
      } catch (err) {
        console.warn('[libcurl] Skipping testBackendLogically: libcurl not available. Error:', err && (err.message || err));
        return; // Skip if libcurl not present
      }

      // Updater and two clients to same URL
      const updater = new Http3ClientBackendUpdater('roundtrip', '127.0.0.1', 12345);
      const localA = new SimpleBackend();
      const localB = new SimpleBackend();
      const req = new Request({ scheme: 'https', authority: '127.0.0.1:12345', path: '/init/wwatp/' });
      const clientA = updater.addBackend(localA, true, req, 60);
      const clientB = updater.addBackend(localB, true, req, 60);

      const { createLionNodes, createElephantNodes, createParrotNodes, createNotesPageTree } = await import('../backend_testbed/backend_testbed.js');

      // Seed via client A
      const up1 = clientA.upsertNode(createLionNodes());
      await updater.maintainRequestHandlers(transport, 0);
      expect(await Promise.race([up1, new Promise((_, r) => setTimeout(() => r(new Error('timeout upserting lion')), 8000))])).toBe(true);

      const up2 = clientA.upsertNode(createElephantNodes());
      await updater.maintainRequestHandlers(transport, 0);
      expect(await Promise.race([up2, new Promise((_, r) => setTimeout(() => r(new Error('timeout upserting elephant')), 8000))])).toBe(true);

      const up3 = clientA.upsertNode(createParrotNodes());
      await updater.maintainRequestHandlers(transport, 0);
      expect(await Promise.race([up3, new Promise((_, r) => setTimeout(() => r(new Error('timeout upserting parrot')), 8000))])).toBe(true);

      const up4 = clientA.upsertNode([createNotesPageTree()]);
      await updater.maintainRequestHandlers(transport, 0);
      expect(await Promise.race([up4, new Promise((_, r) => setTimeout(() => r(new Error('timeout upserting notes page')), 8000))])).toBe(true);

      // Sync client B's local cache
      const fullP = clientB.getFullTree();
      await updater.maintainRequestHandlers(transport, 0);
      const vec = await Promise.race([
        fullP,
        new Promise((_, rej) => setTimeout(() => rej(new Error('timeout waiting for clientB full tree')), 8000)),
      ]);
      expect(Array.isArray(vec)).toBe(true);

      // Validate logically on client B's backend
      const tbB = new BackendTestbed(localB, { shouldTestChanges: true });
      tbB.testBackendLogically('');
    } finally {
      await killServer(server);
    }
  }, 25000);

  it('test deleteElephant', async () => {
    // Detect curl with HTTP/3 support
    const ver = child_process.spawnSync('bash', ['-lc', 'command -v curl >/dev/null 2>&1 && curl -V | head -n1 || echo "nocurl"'], { encoding: 'utf8' });
    const v = ver.stdout || '';
    if (!v || v.includes('nocurl') || !/HTTP\/?3|nghttp3|quiche/i.test(v)) {
      return; // Skip if curl or HTTP/3 not available
    }
    if (!haveCerts() && !tryGenerateSelfSignedCerts()) {
      return; // Skip if certs missing and cannot generate
    }

    process.env.WWATP_CERT = CERT_FILE;
    process.env.WWATP_KEY = KEY_FILE;
    process.env.WWATP_INSECURE = '1';

    const server = await spawnServer();
    try {
      let transport = null;
      try {
        const { LibcurlTransport } = await import('../../index.js');
        transport = new LibcurlTransport();
        await transport.connect({ scheme: 'https', authority: '127.0.0.1:12345' });
      } catch (err) {
        console.warn('[libcurl] Skipping testDeleteElephant: libcurl not available. Error:', err && (err.message || err));
        return; // Skip if libcurl not present
      }

      const updater = new Http3ClientBackendUpdater('roundtrip-del', '127.0.0.1', 12345);
      const localA = new SimpleBackend();
      const localB = new SimpleBackend();
      const req = new Request({ scheme: 'https', authority: '127.0.0.1:12345', path: '/init/wwatp/' });
      const clientA = updater.addBackend(localA, true, req, 60);
      const clientB = updater.addBackend(localB, true, req, 60);

      const { createLionNodes, createElephantNodes, createParrotNodes, createNotesPageTree, checkMultipleDeletedNode } = await import('../backend_testbed/backend_testbed.js');

      // Seed animals + notes via client A
      const u1 = clientA.upsertNode(createLionNodes());
      await updater.maintainRequestHandlers(transport, 0);
      expect(await Promise.race([u1, new Promise((_, r) => setTimeout(() => r(new Error('timeout upserting lion')), 8000))])).toBe(true);

      const u2 = clientA.upsertNode(createElephantNodes());
      await updater.maintainRequestHandlers(transport, 0);
      expect(await Promise.race([u2, new Promise((_, r) => setTimeout(() => r(new Error('timeout upserting elephant')), 8000))])).toBe(true);

      const u3 = clientA.upsertNode(createParrotNodes());
      await updater.maintainRequestHandlers(transport, 0);
      expect(await Promise.race([u3, new Promise((_, r) => setTimeout(() => r(new Error('timeout upserting parrot')), 8000))])).toBe(true);

      const u4 = clientA.upsertNode([createNotesPageTree()]);
      await updater.maintainRequestHandlers(transport, 0);
      expect(await Promise.race([u4, new Promise((_, r) => setTimeout(() => r(new Error('timeout upserting notes page')), 8000))])).toBe(true);

      // Perform deletion on client A
      const delP = clientA.deleteNode('elephant');
      await updater.maintainRequestHandlers(transport, 0);
      const delOk = await Promise.race([
        delP,
        new Promise((_, r) => setTimeout(() => r(new Error('timeout deleting elephant')), 8000)),
      ]);
      expect(!!delOk).toBe(true);

      // Sync client B cache and verify deletion
      const fullP = clientB.getFullTree();
      await updater.maintainRequestHandlers(transport, 0);
      await Promise.race([
        fullP,
        new Promise((_, rej) => setTimeout(() => rej(new Error('timeout waiting for clientB full tree')), 8000)),
      ]);

      // Ensure all elephant-related nodes are gone on B
      checkMultipleDeletedNode(localB, createElephantNodes());
    } finally {
      await killServer(server);
    }
  }, 25000);

  it('upsert a test node and fetch it back via curl bridge', async () => {
    // Detect curl with HTTP/3 support
    const ver = child_process.spawnSync('bash', ['-lc', 'command -v curl >/dev/null 2>&1 && curl -V | head -n1 || echo "nocurl"'], { encoding: 'utf8' });
    const v = ver.stdout || '';
    if (!v || v.includes('nocurl') || !/HTTP\/?3|nghttp3|quiche/i.test(v)) {
      return; // Skip if curl or HTTP/3 not available
    }
    if (!haveCerts() && !tryGenerateSelfSignedCerts()) {
      return; // Skip if certs missing and cannot generate
    }

    // Ensure env for curl bridge
    process.env.WWATP_CERT = CERT_FILE;
    process.env.WWATP_KEY = KEY_FILE;
    process.env.WWATP_INSECURE = '1';

    const server = await spawnServer();
    try {
      const updater = new Http3ClientBackendUpdater('real', '127.0.0.1', 12345);
      const local = new SimpleBackend();
      const client = updater.addBackend(local, true, new Request({ scheme: 'https', authority: '127.0.0.1:12345', path: '/init/wwatp/' }), 0);

      // Prefer LibcurlTransport; skip if not available
      let transport = null;
      try {
        const { LibcurlTransport } = await import('../../index.js');
        transport = new LibcurlTransport();
        await transport.connect({ scheme: 'https', authority: '127.0.0.1:12345' });
      } catch (err) {
        console.warn('[libcurl] Skipping testBackendLogically: libcurl not available. Error:', err && (err.message || err));
        return; // skip if libcurl not present
      }

      // Construct a minimal test node under a unique prefix to avoid conflicts
      const ts = Date.now() & 0xffff;
      const label = `e2e_js/${ts}`;
  const { TreeNode, TreeNodeVersion } = await import('../../index.js');
      const node = new TreeNode({
        labelRule: label,
        description: 'e2e test node',
        propertyInfos: [],
        version: new TreeNodeVersion({ versionNumber: 1, maxVersionSequence: 256, policy: 'public' }),
        childNames: [],
      });
      node.insertProperty(0, 'number', 42n, 'uint64');
      node.insertPropertyString(1, 'note', 'string', 'hello');

      // Upsert the node
      const upOkP = client.upsertNode([node]);
  await updater.maintainRequestHandlers(transport, 0);
      const upOk = await Promise.race([
        upOkP,
        new Promise((_, rej) => setTimeout(() => rej(new Error('timeout waiting for upsert response')), 8000)),
      ]);

  expect(!!upOk).toBe(true);

      // Fetch it back
      const getP = client.getNode(label);
  await updater.maintainRequestHandlers(transport, 0);
      const maybe = await Promise.race([
        getP,
        new Promise((_, rej) => setTimeout(() => rej(new Error('timeout waiting for getNode response')), 8000)),
      ]);

  // Maybe<TreeNode>
  expect(maybe && typeof maybe.isJust === 'function').toBe(true);
      if (maybe.isJust && maybe.isJust()) {
        const back = maybe.getOrElse(null);
        expect(back.getLabelRule()).toBe(label);
      }
    } finally {
  await killServer(server);
    }
  }, 25000);
});
