import { describe, it, expect } from 'vitest';
import { WebTransportCommunication, Request, SimpleBackend, Http3ClientBackendUpdater } from '../../index.js';
import { BackendTestbed } from '../backend_testbed/backend_testbed.js';
import { createWWATPHandler } from './server_mock.js';

// Polyfilled WebTransport that allows installing a request handler.
// Each bidi stream buffers written bytes; on writer.close() it invokes the handler
// and exposes the returned bytes via the reader once, then EOF.
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
          // simulate pending read until writer closes
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

describe('System (WebTransportCommunication + polyfill) – backend logical test', () => {
  it('runs BackendTestbed over WebTransportCommunication end-to-end', async () => {
    const orig = globalThis.WebTransport;
    globalThis.WebTransport = PolyfillWebTransport;
    try {
      // Server-side state and handler
      const serverBackend = new SimpleBackend();
      const handler = createWWATPHandler(serverBackend);
      PolyfillWebTransport.setHandler((_req, data) => handler(_req, data));

      // Client communication and updater
      const comm = new WebTransportCommunication('https://polyfill.local/transport');
      await comm.connect();

      const updater = new Http3ClientBackendUpdater('wt-poly', 'local', 0);
      const localA = new SimpleBackend();
      const req = new Request({ path: '/api/wwatp' });
      const clientA = updater.addBackend(localA, true, req, 120);

      const maintain = async () => { await updater.maintainRequestHandlers(comm, 0); };

      // Populate server with animals and notes
      const tbServer = new BackendTestbed(serverBackend);
      tbServer.addAnimalsToBackend();
      tbServer.addNotesPageTree();

      // Drive one full tree sync first
      const full = clientA.getFullTree();
      await maintain();
      const vec = await full;
      expect(Array.isArray(vec)).toBe(true);

      // Run the logical checks against the client backend
      const tbClientA = new BackendTestbed(localA, { shouldTestChanges: true });
      tbClientA.testBackendLogically('');

      await comm.close();
    } finally {
      if (orig) globalThis.WebTransport = orig; else delete globalThis.WebTransport;
    }
  });
});
