import { describe, it, expect } from 'vitest';
import { createWebTransportConnector } from '../index.js';
import fs from 'node:fs';
import path from 'node:path';

function getCertPaths() {
  const ROOT = path.resolve(__dirname, '../..');
  const data = path.join(ROOT, 'test_instances', 'data');
  const cert = path.join(data, 'cert.pem');
  const key = path.join(data, 'private_key.pem');
  if (fs.existsSync(cert) && fs.existsSync(key)) return { cert, key };
  return null;
}

describe('NodeWebTransportEmulator', () => {
  it('constructs and exposes promises', async () => {
    if (typeof process === 'undefined' || !process.versions?.node) return;
  const certs = getCertPaths();
  expect(certs).toBeTruthy();
  process.env.WWATP_CERT = certs.cert;
  process.env.WWATP_KEY = certs.key;
  let wt;
  wt = createWebTransportConnector('https://127.0.0.1:65535/test');
    expect(wt).toBeTruthy();
    expect(typeof wt.ready?.then).toBe('function');
    expect(typeof wt.closed?.then).toBe('function');
    expect(typeof wt.draining?.then).toBe('function');
  // Do not await ready; session creation may fail if no server is listening.
    await wt.close();
  });
});
