// System-level test: WebTransportCommunication against real WWATP server via Node WebTransport emulator.
// Skipped unless WWATP_E2E=1. Requires built server at build/wwatp_server and QUIC addon built.

import { describe, it, expect } from 'vitest';
import fs from 'fs';
import path from 'path';
import dgram from 'dgram';
import child_process from 'child_process';

import {
  WebTransportCommunication,
  Request,
  Http3ClientBackendUpdater,
  SimpleBackend,
  TreeNode,
  TreeNodeVersion,
  tryLoadNativeQuic,
} from '../../index.js';

// Previously gated on env flags; now we always execute the suite and rely on
// in-test prechecks to early-return when prerequisites aren't available.
const e2e = (process.env.WWATP_E2E || '').toString().toLowerCase();
const full = (process.env.WWATP_E2E_FULL || '').toString().toLowerCase();
console.info('[system_webtransport_real_server.test] WWATP_E2E=%s (no skip gate)', process.env.WWATP_E2E);

const ROOT = path.resolve(__dirname, '../../..');
const SERVER_BIN = path.join(ROOT, 'build', 'wwatp_server');
const CONFIG = path.join(ROOT, 'js_client_lib', 'test', 'resources', 'wwatp_server_config.yaml');
const DATA_DIR = path.join(ROOT, 'test_instances', 'data');
const SANDBOX_DIR = path.join(ROOT, 'test_instances', 'sandbox');
const CERT_FILE = path.join(DATA_DIR, 'cert.pem');
const KEY_FILE = path.join(DATA_DIR, 'private_key.pem');

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
    const done = (ok) => { if (!settled) { settled = true; resolve(ok); } };
    const fail = (err) => { if (!settled) { settled = true; reject(err); } };
    proc.once('exit', (code) => fail(new Error(`server exited before ready, code=${code}`)));
    const iv = setInterval(async () => {
      if (Date.now() - start > timeoutMs) { clearInterval(iv); fail(new Error('timeout waiting for UDP bind')); return; }
      const inUse = await isUdpPortInUse(port, host);
      if (inUse) { clearInterval(iv); done(true); }
    }, 200);
  });
}

async function spawnServer() {
  if (!fs.existsSync(SERVER_BIN)) throw new Error(`Server binary not found at ${SERVER_BIN}`);
  ensureSandboxAndDataDirs();
  const proc = child_process.spawn(SERVER_BIN, [CONFIG], { cwd: ROOT });
  proc.on('error', () => {});
  // Pipe server output for diagnostics during the test
  try {
    proc.stdout?.setEncoding?.('utf-8');
    proc.stderr?.setEncoding?.('utf-8');
    proc.stdout?.on?.('data', (d) => { try { process.stdout.write(`[server stdout] ${d}`); } catch {} });
    proc.stderr?.on?.('data', (d) => { try { process.stderr.write(`[server stderr] ${d}`); } catch {} });
  } catch {}
  await waitForServerReady(proc, 12345, '127.0.0.1', 15000);
  return proc;
}

function killServer(proc, signal = 'SIGTERM', timeoutMs = 5000) {
  if (!proc) return Promise.resolve();
  return new Promise((resolve) => {
    let settled = false;
    const done = () => { if (!settled) { settled = true; resolve(); } };
    try {
      proc.once('exit', () => done());
      proc.kill(signal);
      setTimeout(() => done(), timeoutMs);
    } catch (_) { done(); }
  });
}

const wtDescribe = describe.sequential;

wtDescribe('System (WebTransport real server)', () => {
  it('upsert a test node and fetch it back via WebTransportCommunication', async () => {
    // Pre-checks
    if (!fs.existsSync(SERVER_BIN)) {
      return; // skip silently if no server built
    }
    // QUIC addon required for Node emulator
    const addon = tryLoadNativeQuic();
    if (!addon) {
      console.warn('[webtransport-e2e] Native QUIC addon not available; build js_client_lib/native first.');
      return;
    }

    if (!haveCerts() && !tryGenerateSelfSignedCerts()) {
      return; // skip if no certs and cannot generate
    }

  // Provide mTLS env for emulator
    process.env.WWATP_CERT = CERT_FILE;
    process.env.WWATP_KEY = KEY_FILE;
    process.env.WWATP_INSECURE = '1'; // allow self-signed
    process.env.WWATP_TRACE = process.env.WWATP_TRACE || '1'; // enable tracing

  // Polyfill WebTransport global with Node emulator class
    const { default: NodeWebTransportEmulator } = await import('../../transport/node_webtransport_emulator.js');
    const orig = globalThis.WebTransport;
    globalThis.WebTransport = NodeWebTransportEmulator;

  const server = await spawnServer();
    try {
      // Create WebTransportCommunication pointed at WWATP entry path
      const url = 'https://127.0.0.1:12345/init/wwatp/';
      const comm = new WebTransportCommunication(url);
      await comm.connect();

      // Updater + backend
      const updater = new Http3ClientBackendUpdater('wt', '127.0.0.1', 12345);
      const local = new SimpleBackend();
      const be = updater.addBackend(
        local,
        true,
        new Request({ scheme: 'https', authority: '127.0.0.1:12345', path: '/init/wwatp/' }),
        60
      );

      // Build a simple node and upsert
      const node = new TreeNode({
        labelRule: 'e2e_webtransport_node',
        description: 'hello from webtransport e2e',
        propertyInfos: [],
        version: new TreeNodeVersion({ versionNumber: 1, maxVersionSequence: 256, policy: 'public' }),
        childNames: [],
      });
      // Insert properties and values
      node.insertProperty(0, 'popularity', 42n, 'uint64');
      node.insertPropertyString(1, 'diet', 'string', 'omnivore');

      // Blocking-mode pattern (like libcurl tests):
      // 1) queue request -> 2) call maintainRequestHandlers(connector) once -> 3) await with timeout
      const upOkP = be.upsertNode([node]);
      await updater.maintainRequestHandlers(comm, 0);
      const upOk = await Promise.race([
        upOkP,
        new Promise((_, rej) => setTimeout(() => rej(new Error('timeout waiting for upsert response')), 8000)),
      ]);
      expect(!!upOk).toBe(true);

      // Fetch it back
      const getP = be.getNode('e2e_webtransport_node');
      await updater.maintainRequestHandlers(comm, 0);
      const maybe = await Promise.race([
        getP,
        new Promise((_, rej) => setTimeout(() => rej(new Error('timeout waiting for getNode response')), 8000)),
      ]);
      expect(maybe && typeof maybe.isJust === 'function').toBe(true);
      expect(maybe && maybe.isJust && maybe.isJust()).toBe(true);

      // Optional: verify description round-trip (server may adjust versions)
      const got = maybe.getOrElse(null);
      expect(got.getDescription()).toBe('hello from webtransport e2e');

      await comm.close();
    } finally {
  await killServer(server);
      // Restore global
      try { if (orig) globalThis.WebTransport = orig; else delete globalThis.WebTransport; } catch {}
    }
  }, 5000);
});
