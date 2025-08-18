import { describe, it, expect } from 'vitest';
import { TreeNodeVersion } from '../interface/tree_node.js';

describe('TreeNodeVersion', () => {
  it('defaults and isDefault', () => {
    const v = new TreeNodeVersion();
    expect(v.versionNumber).toBe(0);
    expect(v.maxVersionSequence).toBe(256);
    expect(v.policy).toBe('default');
    expect(v.isDefault()).toBe(true);
  });

  it('increment wraps around', () => {
    const v = new TreeNodeVersion({ versionNumber: 255, maxVersionSequence: 256 });
    v.increment();
    expect(v.versionNumber).toBe(0);
  });

  it('wrap-around comparisons', () => {
    const max = 256;
    const a = new TreeNodeVersion({ versionNumber: 250, maxVersionSequence: max });
    const b = new TreeNodeVersion({ versionNumber: 5, maxVersionSequence: max });
    expect(a.lt(b)).toBe(true); // across wrap small delta
    expect(b.gt(a)).toBe(true);
    expect(a.eq(a)).toBe(true);
    expect(a.le(a)).toBe(true);
  });
});
