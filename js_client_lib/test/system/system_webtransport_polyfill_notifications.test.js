import { describe, it, expect } from 'vitest';
import { WebTransportCommunication, Request, SimpleBackend, Http3ClientBackendUpdater } from '../../index.js';
import { createWWATPHandler } from './server_mock.js';
import { createLionNodes } from '../backend_testbed/backend_testbed.js';

class PolyfillWebTransport {
  constructor(url) { this.url = url; this.ready = Promise.resolve(); this.closed = new Promise((r) => { this._closed = r; }); }
  static setHandler(fn) { PolyfillWebTransport._handler = fn; }
  async close() { this._closed?.(); }
  async createBidirectionalStream() {
    const state = { chunks: [], closed: false, responded: false, response: null };
    const writer = {
      async write(chunk) { state.chunks.push(chunk instanceof Uint8Array ? chunk : new Uint8Array(chunk)); },
      async close() {
        state.closed = true;
        const total = state.chunks.reduce((s, c) => s + c.byteLength, 0);
        const body = new Uint8Array(total);
        let o = 0; for (const c of state.chunks) { body.set(c, o); o += c.byteLength; }
        const h = PolyfillWebTransport._handler; state.response = h ? await h({ url: this.url }, body) : new Uint8Array(0);
      },
      async abort() { state.closed = true; },
      releaseLock() {},
    };
    const reader = {
      async read() {
        if (!state.closed) { await new Promise((r) => setTimeout(r, 5)); if (!state.closed) return { value: undefined, done: false }; }
        if (state.responded) return { value: undefined, done: true };
        state.responded = true;
        return { value: state.response instanceof Uint8Array ? state.response : new Uint8Array(0), done: false };
      },
      async cancel() { state.closed = true; },
      releaseLock() {},
    };
    return { writable: { getWriter: () => writer }, readable: { getReader: () => reader } };
  }
}

describe('System (WebTransportCommunication + polyfill) – peer notifications', () => {
  it('registers listener, observes create/delete via journal, then stops after deregister', async () => {
    const orig = globalThis.WebTransport;
    globalThis.WebTransport = PolyfillWebTransport;
    try {
      const serverBackend = new SimpleBackend();
      PolyfillWebTransport.setHandler(createWWATPHandler(serverBackend));

      const comm = new WebTransportCommunication('https://polyfill.local/transport');
      await comm.connect();

      const updater = new Http3ClientBackendUpdater('wt-notif', 'local', 0);
      const localA = new SimpleBackend();
      const localB = new SimpleBackend();
      const req = new Request({ path: '/api/wwatp' });
      const A = updater.addBackend(localA, true, req, 120);
      updater.addBackend(localB, true, req, 120);

  // Advance logical time on each maintain to trigger journal intervals
  let t = 0;
  const maintain = async () => { t += 1; await updater.maintainRequestHandlers(comm, t); };

      const seen = [];
      A.registerNodeListener('listenerA', 'lion', false, (_backend, labelRule, maybeNode) => {
        seen.push([labelRule, typeof maybeNode?.isJust === 'function' ? maybeNode.isJust() : false]);
      });
      await maintain();

      // Create lion nodes on server (as if via peer)
      serverBackend.upsertNode(createLionNodes());
  await maintain(); // journal tick should pull notification (time advanced)
  await new Promise((r) => setTimeout(r, 5));

      // Delete lion on server
      serverBackend.deleteNode('lion');
  await maintain(); // another tick for delete
  await new Promise((r) => setTimeout(r, 5));

      expect(seen.length >= 2).toBe(true);
      expect(seen[0][0]).toBe('lion');
      expect(seen[0][1]).toBe(true);
      expect(seen[seen.length - 1][0]).toBe('lion');
      expect(seen[seen.length - 1][1]).toBe(false);

      // Deregister and ensure no more notifications
      A.deregisterNodeListener('listenerA', 'lion');
  await maintain();
  await new Promise((r) => setTimeout(r, 5));
      const before = seen.length;
      serverBackend.upsertNode(createLionNodes());
  await maintain();
  await new Promise((r) => setTimeout(r, 5));
      serverBackend.deleteNode('lion');
      await maintain();
      expect(seen.length).toBe(before);

      await comm.close();
    } finally {
      if (orig) globalThis.WebTransport = orig; else delete globalThis.WebTransport;
    }
  });
});
