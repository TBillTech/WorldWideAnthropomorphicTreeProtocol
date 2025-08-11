import { describe, it, expect } from 'vitest';
import { Maybe, Just, Nothing, fromNullable } from '../index.js';

describe('Maybe', () => {
  it('constructs Just and Nothing', () => {
    const j = Just(5);
    expect(j.isJust()).toBe(true);
    expect(j.isNothing()).toBe(false);
    expect(j.getOrElse(0)).toBe(5);

    expect(Nothing.isNothing()).toBe(true);
    expect(Nothing.getOrElse(7)).toBe(7);
  });

  it('map and flatMap', () => {
    const j = Just(2).map(x => x + 3);
    expect(j.isJust()).toBe(true);
    expect(j.getOrElse(0)).toBe(5);

    const fm = Just(2).flatMap(x => Just(x * 10));
    expect(fm.getOrElse(0)).toBe(20);

    const stayNothing = Nothing.map(x => x + 1).flatMap(() => Just(9));
    expect(stayNothing.isNothing()).toBe(true);
  });

  it('getOrElse with function', () => {
    const d1 = Nothing.getOrElse(() => 42);
    expect(d1).toBe(42);

    const d2 = Just(1).getOrElse(() => 99);
    expect(d2).toBe(1);
  });

  it('orElse accepts Maybe or thunk', () => {
    const a = Nothing.orElse(Just('x'));
    expect(a.isJust()).toBe(true);
    expect(a.getOrElse('')).toBe('x');

    const b = Nothing.orElse(() => Just('y'));
    expect(b.getOrElse('')).toBe('y');

    const c = Just('z').orElse(Just('w'));
    expect(c.getOrElse('')).toBe('z');
  });

  it('fromNullable', () => {
    expect(fromNullable(null).isNothing()).toBe(true);
    expect(fromNullable(undefined).isNothing()).toBe(true);
    expect(fromNullable(0).isJust()).toBe(true);
    expect(fromNullable('').isJust()).toBe(true);
  });

  it('equals with and without comparator', () => {
    expect(Just(1).equals(Just(1))).toBe(true);
    expect(Just(1).equals(Just(2))).toBe(false);
    expect(Nothing.equals(Nothing)).toBe(true);
    expect(Just({ a: 1 }).equals(Just({ a: 1 }))).toBe(false); // ref equality
    const cmp = (x, y) => JSON.stringify(x) === JSON.stringify(y);
    expect(Just({ a: 1 }).equals(Just({ a: 1 }), cmp)).toBe(true);
  });

  it('flatMap requires Maybe return', () => {
    expect(() => Just(1).flatMap(() => 2)).toThrowError();
  });

  it('toJSON and toString', () => {
    expect(JSON.stringify(Just(3))).toBe(JSON.stringify({ kind: 'just', value: 3 }));
    expect(JSON.stringify(Nothing)).toBe(JSON.stringify({ kind: 'nothing' }));
    expect(String(Just(4))).toBe('Just(4)');
    expect(String(Nothing)).toBe('Nothing');
  });
});
