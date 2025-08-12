import { describe, it, expect } from 'vitest';
import { StreamIdentifier, Request, MockCommunication } from '../index.js';

describe('StreamIdentifier', () => {
    it('compares and stringifies', () => {
        const a = new StreamIdentifier('cid', 1);
        const b = new StreamIdentifier('cid', 2);
        expect(a.equals(a)).toBe(true);
        expect(a.equals(b)).toBe(false);
        expect(a.compare(b)).toBeLessThan(0);
        expect(String(a)).toBe('cid:1');
    });
});

describe('Request', () => {
    it('equality and WWATP detection', () => {
        const r1 = new Request({ path: '/api/wwatp/get' });
        const r2 = new Request({ path: '/api/wwatp/get' });
        const r3 = new Request({ path: '/other' });
        expect(r1.equals(r2)).toBe(true);
        expect(r1.isWWATP()).toBe(true);
        expect(r3.isWWATP()).toBe(false);
    });
});

describe('MockCommunication', () => {
    it('routes response to handler', async () => {
        const comm = new MockCommunication();
        const sid = comm.getNewRequestStreamIdentifier();
        const req = new Request({ path: '/api/wwatp/ping', method: 'POST' });
        comm.setMockHandler((_req, data) => {
            // echo
            const out = new Uint8Array(data.length + 1);
            out.set(data);
            out[out.length - 1] = 0x7f;
            return out;
        });
        let got;
        comm.registerResponseHandler(sid, (evt) => {
            got = evt;
        });
        await comm.sendRequest(sid, req, new Uint8Array([1, 2, 3]));
        expect(got).toBeDefined();
        expect(got.ok).toBe(true);
        expect(Array.from(got.data)).toEqual([1, 2, 3, 127]);
    });
});
