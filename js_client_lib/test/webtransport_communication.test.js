import { describe, it, expect } from 'vitest';
import { WebTransportCommunication, StreamIdentifier, Request } from '../index.js';

class FakeWebTransport {
  constructor(url) {
    this.url = url;
    this.ready = Promise.resolve();
    this._closedResolve = null;
    this.closed = new Promise((res) => { this._closedResolve = res; });
  }
  async close() { this._closedResolve?.(); }
  async createBidirectionalStream() {
    const state = { chunks: [], closed: false, readOnce: false };
    const writer = {
      async write(chunk) { state.chunks.push(chunk instanceof Uint8Array ? chunk : new Uint8Array(chunk)); },
      async close() { state.closed = true; },
      async abort() { state.closed = true; },
      releaseLock() {}
    };
    const reader = {
      async read() {
        if (!state.closed) {
          // simulate pending read until writer closes; tiny delay
          await new Promise(r => setTimeout(r, 5));
          if (!state.closed) return { value: undefined, done: false };
        }
        if (state.readOnce) return { value: undefined, done: true };
        const total = state.chunks.reduce((s, c) => s + c.byteLength, 0);
        const out = new Uint8Array(total);
        let o = 0; for (const c of state.chunks) { out.set(c, o); o += c.byteLength; }
        state.readOnce = true;
        return { value: out, done: false };
      },
      async cancel() { state.closed = true; },
      releaseLock() {}
    };
    return {
      writable: { getWriter: () => writer },
      readable: { getReader: () => reader },
    };
  }
}

describe('WebTransportCommunication (polyfilled)', () => {
  it('writes and reads back exact bytes', async () => {
    const orig = globalThis.WebTransport;
    globalThis.WebTransport = FakeWebTransport;
    try {
      const comm = new WebTransportCommunication('https://example.com/transport');
      await comm.connect();
      const req = new Request({ path: '/api/wwatp/ping', method: 'POST' });
      const sid = comm.getNewRequestStreamIdentifier(req);
      const sent = new Uint8Array([1,2,3,4]);
      let got;
      comm.registerResponseHandler(sid, (evt) => { got = evt; });
      const r = await comm.sendRequest(sid, req, sent);
      expect(r.ok).toBe(true);
      expect(Array.from(got.data)).toEqual(Array.from(sent));
      await comm.close();
    } finally {
      if (orig) globalThis.WebTransport = orig; else delete globalThis.WebTransport;
    }
  });

  it('honors timeout via options.timeoutMs', async () => {
    const orig = globalThis.WebTransport;
    globalThis.WebTransport = class extends FakeWebTransport {
      async createBidirectionalStream() {
        // Override: never close writer to force timeout
        const base = await super.createBidirectionalStream();
        const writer = base.writable.getWriter();
        base.writable.getWriter = () => ({
          async write(c) { return writer.write(c); },
          async close() { await new Promise(() => {}); },
          async abort() { /* swallow */ },
          releaseLock() {}
        });
        return base;
      }
    };
    try {
      const comm = new WebTransportCommunication('https://example.com/transport');
      await comm.connect();
      const req = new Request({ path: '/api/wwatp/ping', method: 'POST' });
      const sid = comm.getNewRequestStreamIdentifier(req);
      const ac = new AbortController();
      let threw = false;
      try {
        await comm.sendRequest(sid, req, new Uint8Array([9,9,9]), { timeoutMs: 10, signal: ac.signal });
      } catch (e) {
        threw = true;
      }
      expect(threw).toBe(true);
      await comm.close();
    } finally {
      if (orig) globalThis.WebTransport = orig; else delete globalThis.WebTransport;
    }
  });
});
