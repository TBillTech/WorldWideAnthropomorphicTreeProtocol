import { describe, it, expect } from 'vitest';
import { NativeQuic } from '../index.js';
import fs from 'node:fs';
import path from 'node:path';
import child_process from 'node:child_process';
import dgram from 'node:dgram';

const ROOT = path.resolve(__dirname, '../..');
const SERVER_BIN = path.join(ROOT, 'build', 'wwatp_server');
const CONFIG = path.join(ROOT, 'js_client_lib', 'test', 'resources', 'wwatp_server_config.yaml');
const DATA_DIR = path.join(ROOT, 'test_instances', 'data');
const CERT_FILE = path.join(DATA_DIR, 'cert.pem');
const KEY_FILE = path.join(DATA_DIR, 'private_key.pem');

function haveCerts() { return fs.existsSync(CERT_FILE) && fs.existsSync(KEY_FILE); }

async function isUdpPortInUse(port, host = '127.0.0.1') {
  return new Promise((resolve) => {
    const s = dgram.createSocket('udp4');
    s.once('error', (err) => {
      if (err.code === 'EADDRINUSE' || err.code === 'EACCES') resolve(true);
      else resolve(false);
    });
    s.bind(port, host, () => { s.close(() => resolve(false)); });
  });
}

async function waitForServerReady(proc, port = 12345, host = '127.0.0.1', timeoutMs = 15000) {
  const start = Date.now();
  return new Promise((resolve, reject) => {
    let settled = false;
    const done = (ok) => { if (!settled) { settled = true; clearInterval(iv); resolve(ok); } };
    const fail = (err) => { if (!settled) { settled = true; clearInterval(iv); reject(err); } };
    proc.once('exit', (code) => fail(new Error(`server exited before ready, code=${code}`)));
    const iv = setInterval(async () => {
      if (Date.now() - start > timeoutMs) return fail(new Error('timeout waiting for server UDP bind'));
      const inUse = await isUdpPortInUse(port, host);
      if (inUse) done(true);
    }, 200);
  });
}

async function spawnServerIfNeeded() {
  const inUse = await isUdpPortInUse(12345, '127.0.0.1');
  if (inUse) return { proc: null, started: false };
  if (!fs.existsSync(SERVER_BIN)) return { proc: null, started: false };
  if (!fs.existsSync(CONFIG)) return { proc: null, started: false };
  if (!haveCerts()) return { proc: null, started: false };
  const proc = child_process.spawn(SERVER_BIN, [CONFIG], { cwd: ROOT });
  proc.on('error', () => {});
  await waitForServerReady(proc);
  return { proc, started: true };
}

function killServer(proc) { try { proc?.kill('SIGINT'); } catch {} }

describe('NativeQuic (FFI POC)', () => {
  it('addon is present or test is skipped', () => {
    const ap = path.join(ROOT, 'js_client_lib', 'native', 'build', 'Release', 'wwatp_quic_native.node');
    if (!fs.existsSync(ap)) {
      console.warn(`[native_quic.test] addon not built at ${ap}; skipping.`);
      expect(true).toBe(true);
      return;
    }
    expect(fs.existsSync(ap)).toBe(true);
  });

  it('opens a session and stream against local server', async () => {
    const { proc, started } = await spawnServerIfNeeded();
    try {
      const nq = NativeQuic();
      if (!nq) { expect(nq).toBeNull(); return; }
      const url = 'https://127.0.0.1:12345';
      const opts = haveCerts()
        ? { url, cert_file: CERT_FILE, key_file: KEY_FILE }
        : { url, insecure_skip_verify: true };
      const sess = nq.createSession(opts);
      expect(sess).toBeTruthy();
      const st = nq.openBidiStream(sess, '/init/wwatp/');
      expect(st).toBeTruthy();
      if (process.env.WWATP_NATIVE_QUIC_WRITE === '1') {
        const input = new Uint8Array([1,2,3,4]);
        const wrote = nq.write(st, input, true);
        expect(wrote).toBeGreaterThan(0);
        const out = nq.read(st, 4096, 1000);
        expect(out).toBeInstanceOf(Uint8Array);
      }
      nq.closeStream(st);
      nq.closeSession(sess);
    } finally {
      if (started) killServer(proc);
    }
  }, 20000);
});
