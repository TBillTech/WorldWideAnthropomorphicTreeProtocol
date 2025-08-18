/* eslint-env node, browser */
// Factory to create a WebTransport-like object depending on environment.
// - In browsers with WebTransport: returns the native WebTransport instance
// - In Node with native QUIC addon: returns NodeWebTransportEmulator
// - Otherwise throws (callers should fall back to Fetch or mocks)

import NodeWebTransportEmulator from './node_webtransport_emulator.js';

export default function createWebTransportConnector(url, options = {}) {
  // Prefer real browser WebTransport when available
  try {
    if (typeof globalThis !== 'undefined' && typeof globalThis.WebTransport !== 'undefined') {
      // eslint-disable-next-line no-undef
      return new WebTransport(url, options);
    }
  } catch (e) {
    // ignore feature detection errors
  }
  // Node path: provide emulator if addon available
  if (typeof process !== 'undefined' && process.versions?.node) {
    return new NodeWebTransportEmulator(url, options);
  }
  throw new Error('No WebTransport implementation available in this environment');
}
