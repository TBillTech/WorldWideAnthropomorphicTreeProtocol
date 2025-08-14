import { describe, it, expect } from 'vitest';
import { StreamIdentifier, Request, NodeWebTransportMock } from '../index.js';

describe('NodeWebTransportMock', () => {
  it('connects, routes response, and preserves connectionId', async () => {
    const comm = new NodeWebTransportMock('mock://unit');
    await comm.connect();
    const req = new Request({ path: '/api/wwatp/ping', method: 'POST' });
    const sid = comm.getNewRequestStreamIdentifier(req);
    expect(String(sid)).toMatch(/^node-webtransport:mock:\/\/unit:/);
    comm.setMockHandler((_req, data) => {
      const out = new Uint8Array((data?.length || 0) + 2);
      if (data) out.set(data);
      out[out.length - 2] = 0x55; out[out.length - 1] = 0xaa;
      return out;
    });
    let got;
    comm.registerResponseHandler(sid, (evt) => { got = evt; });
    await comm.sendRequest(sid, req, new Uint8Array([1, 2, 3]));
    expect(got).toBeDefined();
    expect(got.ok).toBe(true);
    expect(Array.from(got.data)).toEqual([1,2,3,0x55,0xaa]);
    await comm.close();
  });
});
