import { describe, it, expect } from 'vitest';
import { WebTransportCommunication, Request, SimpleBackend, Http3ClientBackendUpdater } from '../../index.js';
import { createWWATPHandler } from './server_mock.js';
import { TreeNode, TreeNodeVersion } from '../../index.js';

// Minimal WebTransport polyfill for bidi request/response used by WebTransportCommunication
class PolyfillWebTransport {
  constructor(url) {
    this.url = url;
    this.ready = Promise.resolve();
    this._closedResolve = null;
    this.closed = new Promise((res) => { this._closedResolve = res; });
  }
  static setHandler(fn) { PolyfillWebTransport._handler = fn; }
  async close() { this._closedResolve?.(); }
  async createBidirectionalStream() {
    const state = { chunks: [], closed: false, responded: false, response: null };
    const writer = {
      async write(chunk) { state.chunks.push(chunk instanceof Uint8Array ? chunk : new Uint8Array(chunk)); },
      async close() {
        state.closed = true;
        const total = state.chunks.reduce((s, c) => s + c.byteLength, 0);
        const body = new Uint8Array(total);
        let o = 0; for (const c of state.chunks) { body.set(c, o); o += c.byteLength; }
        try {
          const h = PolyfillWebTransport._handler;
          state.response = h ? await h({ url: this.url }, body) : new Uint8Array(0);
        } catch (_) {
          state.response = new Uint8Array(0);
        }
      },
      async abort() { state.closed = true; },
      releaseLock() {},
    };
    const reader = {
      async read() {
        if (!state.closed) {
          await new Promise((r) => setTimeout(r, 5));
          if (!state.closed) return { value: undefined, done: false };
        }
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

describe('System (WebTransportCommunication + polyfill) – roundtrip', () => {
  it('concurrent getNode requests resolve correctly and missing node yields Nothing', async () => {
    const orig = globalThis.WebTransport;
    globalThis.WebTransport = PolyfillWebTransport;
    try {
      // Server with two nodes seeded
      const serverBackend = new SimpleBackend();
      const n1 = new TreeNode({
        labelRule: 'rt_lion',
        description: 'lion rt',
        propertyInfos: [],
        version: new TreeNodeVersion({ versionNumber: 1, maxVersionSequence: 256, policy: 'public' }),
        childNames: [],
      });
      const n2 = new TreeNode({
        labelRule: 'rt_elephant',
        description: 'elephant rt',
        propertyInfos: [],
        version: new TreeNodeVersion({ versionNumber: 1, maxVersionSequence: 256, policy: 'public' }),
        childNames: [],
      });
      serverBackend.upsertNode([n1, n2]);

      PolyfillWebTransport.setHandler(createWWATPHandler(serverBackend));

      const comm = new WebTransportCommunication('https://polyfill.local/transport');
      await comm.connect();

      const updater = new Http3ClientBackendUpdater('wt-rt', 'local', 0);
      const local = new SimpleBackend();
      const client = updater.addBackend(local, true, new Request({ path: '/api/wwatp' }), 0);

      const maintain = async () => { await updater.maintainRequestHandlers(comm, 0); };

      // Issue concurrent requests
      const p1 = client.getNode('rt_lion');
      const p2 = client.getNode('rt_elephant');
      const p3 = client.getNode('rt_missing');
      await maintain();
      const [m1, m2, m3] = await Promise.all([p1, p2, p3]);

      expect(m1 && m1.isJust && m1.isJust()).toBe(true);
      expect(m1.getOrElse(null).getDescription()).toBe('lion rt');
      expect(m2 && m2.isJust && m2.isJust()).toBe(true);
      expect(m2.getOrElse(null).getDescription()).toBe('elephant rt');
      expect(m3 && m3.isNothing && m3.isNothing()).toBe(true);

      await comm.close();
    } finally {
      if (orig) globalThis.WebTransport = orig; else delete globalThis.WebTransport;
    }
  });
});
