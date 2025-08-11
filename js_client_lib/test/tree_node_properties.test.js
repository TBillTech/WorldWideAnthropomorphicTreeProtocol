import { describe, it, expect } from 'vitest';
import { TreeNode, TreeNodeVersion } from '../interface/tree_node.js';

function makeNode() {
  const propertyInfos = [
    { type: 'double', name: 'mass' },
    { type: 'bool', name: 'isKing' },
    { type: 'string', name: 'notes' },
    { type: 'uint64', name: 'id' },
  ];
  // Build propertyData: double(8) + bool(1) + var(4+N) + uint64(8)
  const enc = new TextEncoder();
  const note = enc.encode('lion');
  const len = 8 + 1 + 4 + note.length + 8;
  const buf = new Uint8Array(len);
  const dv = new DataView(buf.buffer);
  let off = 0;
  dv.setFloat64(off, 190.5, true); off += 8;
  dv.setUint8(off, 1); off += 1;
  dv.setUint32(off, note.length, true); off += 4;
  buf.set(note, off); off += note.length;
  dv.setBigUint64(off, 1234n, true); off += 8;
  return new TreeNode({ labelRule: 'animals/lion', description: 'lion', propertyInfos, propertyData: buf, childNames: ['cubs/c1', 'cubs/c2'] });
}

describe('TreeNode property layout', () => {
  it('reads values correctly', () => {
    const n = makeNode();
    const [s1, mass] = n.getPropertyValue('mass');
    expect(s1).toBe(8);
    expect(mass).toBeCloseTo(190.5, 5);
    const [s2, str] = n.getPropertyString('notes');
    expect(str).toBe('lion');
    const [s3, id] = n.getPropertyValue('id');
    expect(id).toBe(1234n);
  });

  it('set fixed and var values and preserve order', () => {
    const n = makeNode();
    n.setPropertyValue('mass', 200.25);
    n.setPropertyString('notes', 'alpha');
    const [, mass] = n.getPropertyValue('mass');
    expect(mass).toBeCloseTo(200.25, 5);
    const [, notes] = n.getPropertyString('notes');
    expect(notes).toBe('alpha');
  });

  it('insert and delete properties', () => {
    const n = makeNode();
    n.insertProperty(0, 'alive', true, 'bool');
    const [, alive] = n.getPropertyValue('alive');
    expect(alive).toBe(true);
    n.insertPropertyString(100, 'comment', 'string', 'roars');
    const [, comment] = n.getPropertyString('comment');
    expect(comment).toBe('roars');
    n.deleteProperty('isKing');
    expect(() => n.getPropertyValue('isKing')).toThrow();
  });

  it('path helpers and label prefix/shorten', () => {
    const n = makeNode();
    expect(n.getNodeName()).toBe('lion');
    expect(n.getNodePath()).toBe('animals');
    const abs = n.getAbsoluteChildNames();
    expect(abs[0]).toBe('animals/cubs/c1');
    n.prefixLabels('zoo');
    expect(n.getLabelRule()).toBe('zoo/animals/lion');
    n.shortenLabels('zoo');
    expect(n.getLabelRule()).toBe('animals/lion');
  });
});
