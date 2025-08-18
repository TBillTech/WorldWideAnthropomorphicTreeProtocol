import { describe, it, expect } from 'vitest';
import { Just, Nothing } from '../interface/maybe.js';
import { prefixNewNodeVersionLabels, shortenNewNodeVersionLabels, prefixTransactionLabels, shortenTransactionLabels, TreeNode } from '../interface/tree_node.js';

describe('Transaction label helpers', () => {
  it('prefix/shorten for NewNodeVersion', () => {
    const node = new TreeNode({ labelRule: 'a/b' });
    const nnv = [Just(1), ['a/b', Just(node)]];
    const p = prefixNewNodeVersionLabels('root', nnv);
    expect(p[1][0]).toBe('root/a/b');
    expect(p[1][1].value.getLabelRule()).toBe('root/a/b');
    const s = shortenNewNodeVersionLabels('root', p);
    expect(s[1][0]).toBe('a/b');
  });

  it('prefix/shorten for Transaction', () => {
    const n1 = new TreeNode({ labelRule: 'a' });
    const n2 = new TreeNode({ labelRule: 'a/c' });
    const tx = [
      [ [Nothing, ['a', Just(n1)]], [ [Nothing, ['a/c', Just(n2)]] ] ],
    ];
    const p = prefixTransactionLabels('root', tx);
    expect(p[0][0][1][0]).toBe('root/a');
    expect(p[0][1][0][1][0]).toBe('root/a/c');
    const s = shortenTransactionLabels('root', p);
    expect(s[0][0][1][0]).toBe('a');
  });
});
