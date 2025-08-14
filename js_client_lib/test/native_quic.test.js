import { describe, it, expect } from 'vitest';
import { NativeQuic } from '../index.js';

// POC test for the C facade via FFI. Skips if library or ffi modules not available.
describe('NativeQuic (FFI POC)', () => {
  it('loads library and echoes bytes (stub)', async () => {
    const nq = NativeQuic();
    if (!nq) {
      // Skip if lib or ffi missing
      expect(nq).toBeNull();
      return;
    }
    // Create a session to a dummy URL (stub does not connect)
    const sess = nq.createSession({ url: 'https://127.0.0.1:12345' });
    expect(sess).toBeTruthy();
    const st = nq.openBidiStream(sess, '/init/wwatp/');
    expect(st).toBeTruthy();
    const input = new Uint8Array([1,2,3,4]);
    const wrote = nq.write(st, input, true);
    expect(wrote).toBe(4);
    const out = nq.read(st, 4096, 0);
    // Stub echoes back what was written
    expect(Array.from(out)).toEqual([1,2,3,4]);
    nq.closeStream(st);
    nq.closeSession(sess);
  });
});
