import { describe, it, expect } from 'vitest';
import { createRequire } from 'module';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { tryLoadNativeQuic } from '../index.js';

// Replace ffi-napi sanity checks with N-API addon checks

function addonPath() {
  const __filename = fileURLToPath(import.meta.url);
  const __dirname = path.dirname(__filename);
  const ROOT = path.resolve(__dirname, '..'); // js_client_lib
  return path.join(ROOT, 'native', 'build', 'Release', 'wwatp_quic_native.node');
}

describe('napi addon: load and export symbols', () => {
  it('requires the addon and exposes expected functions', async () => {
    const ap = addonPath();
    if (!fs.existsSync(ap)) {
      console.warn(`[ffi_sanity.test] addon not built at ${ap}; skipping (run: cd js_client_lib/native && npm run build)`);
      expect(true).toBe(true);
      return;
    }
    const require = createRequire(import.meta.url);
    const addon = require(ap);
    const keys = Object.keys(addon);
    expect(keys).toEqual(expect.arrayContaining(['lastError','createSession','closeSession','openBidiStream','write','read','closeStream']));
    const msg = addon.lastError();
    expect(typeof msg === 'string').toBe(true);
  });
});

describe('napi addon: loader prefers addon over FFI', () => {
  it('tryLoadNativeQuic returns addon binding', async () => {
    const ap = addonPath();
    if (!fs.existsSync(ap)) {
      console.warn('[ffi_sanity.test] addon not built; skipping loader preference check.');
      expect(true).toBe(true);
      return;
    }
    const nq = tryLoadNativeQuic();
    expect(nq && nq.libPath).toBe('(napi addon)');
  });
});
