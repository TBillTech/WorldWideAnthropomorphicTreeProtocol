import { describe, it, expect } from 'vitest';
import { Communication } from '../index.js';

describe('scaffold smoke', () => {
  it('exports Communication class', () => {
    expect(typeof Communication).toBe('function');
    const c = new Communication();
    expect(() => c.send(new Uint8Array())).toThrowError();
  });
});
