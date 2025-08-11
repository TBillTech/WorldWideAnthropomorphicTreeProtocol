import { describe, it, expect, vi } from 'vitest';
import { SimpleBackend, TreeNode, Just, Nothing, Maybe } from '../index.js';

function makeAnimal(label, desc = '') {
  return new TreeNode({ labelRule: label, description: desc, propertyInfos: [], propertyData: new Uint8Array(0), childNames: [] });
}

function makePageNode(label, list) {
  // Initialize with empty string payload for 'page_nodes' variable-size property
  const init = new Uint8Array(4); // length=0
  const n = new TreeNode({ labelRule: label, description: 'page', propertyInfos: [{ type: 'string', name: 'page_nodes' }], propertyData: init });
  n.setPropertyString('page_nodes', JSON.stringify(list));
  return n;
}

describe('SimpleBackend CRUD and queries', () => {
  it('upsert/get/delete work', () => {
    const b = new SimpleBackend();
    const a = makeAnimal('zoo/lion');
    const e = makeAnimal('zoo/elephant');
    expect(b.getNode('zoo/lion').isNothing()).toBe(true);
    b.upsertNode([a, e]);
    expect(b.getNode('zoo/lion').isJust()).toBe(true);
    expect(b.getNode('zoo/elephant').isJust()).toBe(true);
    expect(b.deleteNode('zoo/elephant')).toBe(true);
    expect(b.getNode('zoo/elephant').isNothing()).toBe(true);
  });

  it('queryNodes supports exact, wildcard, and prefix', () => {
    const b = new SimpleBackend();
    b.upsertNode([
      makeAnimal('zoo/lion'),
      makeAnimal('zoo/tiger'),
      makeAnimal('zoo/birds/parrot'),
      makeAnimal('zoo/birds/eagle'),
    ]);
    expect(b.queryNodes('zoo/lion').length).toBe(1);
    expect(b.queryNodes('zoo/*').length).toBe(2);
    expect(b.queryNodes('zoo/birds/*').length).toBe(2);
    expect(b.queryNodes('zoo/*/*').length).toBe(2);
  });

  it('relativeQueryNodes resolves base path', () => {
    const b = new SimpleBackend();
    const base = makeAnimal('zoo');
    b.upsertNode([base, makeAnimal('zoo/lion'), makeAnimal('zoo/tiger')]);
    expect(b.relativeQueryNodes(base, '*').length).toBe(2);
  });
});

describe('Page tree', () => {
  it('reads page_nodes list and returns nodes', () => {
    const b = new SimpleBackend();
    b.upsertNode([
      makeAnimal('zoo/lion'),
      makeAnimal('zoo/tiger'),
      makePageNode('zoo/pages/animals', ['zoo/lion', 'zoo/tiger']),
    ]);
    const arr = b.getPageTree('zoo/pages/animals');
    const labels = arr.map((n) => n.getLabelRule()).sort();
    expect(labels).toEqual(['zoo/lion', 'zoo/tiger']);
  });

  it('relativeGetPageTree uses base path', () => {
    const b = new SimpleBackend();
    const base = makeAnimal('zoo');
    b.upsertNode([
      base,
      makeAnimal('zoo/lion'),
      makeAnimal('zoo/tiger'),
      makePageNode('zoo/pages/animals', ['zoo/lion', 'zoo/tiger']),
    ]);
    const arr = b.relativeGetPageTree(base, 'pages/animals');
    expect(arr.length).toBe(2);
  });
});

describe('Listeners and notifications', () => {
  it('fires for exact and childNotify overlap', () => {
    const b = new SimpleBackend();
    const fn = vi.fn();
    b.registerNodeListener('L', 'zoo', true, fn);
    b.upsertNode([makeAnimal('zoo/lion')]);
    expect(fn).toHaveBeenCalled();
    b.deleteNode('zoo/lion');
    expect(fn).toHaveBeenCalledTimes(2);
    b.deregisterNodeListener('L', 'zoo');
  });
});

describe('Transactions', () => {
  it('applies subtransactions atomically and notifies', () => {
    const b = new SimpleBackend();
    const notify = vi.fn();
    b.registerNodeListener('L', 'zoo', true, notify);

    const parent = [Nothing, ['zoo/lion', Just(makeAnimal('zoo/lion'))]];
    const cub = [Just(1), ['zoo/lion/cub', Just(makeAnimal('zoo/lion/cub'))]];
    const st = [parent, [cub]];
    const ok = b.applyTransaction([st]);
    expect(ok).toBe(true);
    expect(b.getNode('zoo/lion').isJust()).toBe(true);
    expect(b.getNode('zoo/lion/cub').isJust()).toBe(true);
    expect(notify).toHaveBeenCalled();
  });
});
