import { describe, it, expect } from 'vitest';
import { createTransportForUrl, FetchCommunication } from '../index.js';

describe('createTransportForUrl', () => {
  it('falls back to Fetch when WebTransport not available', () => {
    // Simulate environment without WebTransport
    const origWT = globalThis.WebTransport;
    // ensure undefined
    try { delete globalThis.WebTransport; } catch {}
    const comm = createTransportForUrl('https://example.com/init/wwatp/');
    expect(comm).toBeInstanceOf(FetchCommunication);
    // restore
    if (origWT) globalThis.WebTransport = origWT;
  });

  it('respects preferred=fetch', () => {
    const comm = createTransportForUrl('https://example.com', { preferred: 'fetch' });
    expect(comm).toBeInstanceOf(FetchCommunication);
  });
});
