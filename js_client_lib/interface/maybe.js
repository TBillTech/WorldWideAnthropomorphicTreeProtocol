// Maybe<T> – a tiny, browser-safe tagged union with helpers.
// Shape: { kind: 'just' | 'nothing', value?: T }
// Usage:
//   const m = Just(3).map(x => x + 1); // Just(4)
//   const y = Nothing.getOrElse(42);   // 42

export class Maybe {
  constructor(kind, value) {
    this.kind = kind; // 'just' | 'nothing'
    if (kind === 'just') this.value = value;
    Object.freeze(this);
  }

  static isMaybe(x) {
    return x instanceof Maybe || (x && (x.kind === 'just' || x.kind === 'nothing'));
  }

  static Just(value) {
    return new Maybe('just', value);
  }

  static Nothing() {
    return NOTHING_SINGLETON;
  }

  static fromNullable(value) {
    return value === null || value === undefined ? NOTHING_SINGLETON : new Maybe('just', value);
  }

  isJust() {
    return this.kind === 'just';
  }

  isNothing() {
    return this.kind === 'nothing';
  }

  map(fn) {
    if (this.isNothing()) return this;
    return Maybe.Just(fn(this.value));
  }

  flatMap(fn) {
    if (this.isNothing()) return this;
    const out = fn(this.value);
    if (!Maybe.isMaybe(out)) {
      throw new TypeError('flatMap function must return a Maybe');
    }
    return out;
  }

  getOrElse(defaultValueOrFn) {
    if (this.isJust()) return this.value;
    return typeof defaultValueOrFn === 'function' ? defaultValueOrFn() : defaultValueOrFn;
  }

  orElse(otherMaybeOrFn) {
    if (this.isJust()) return this;
    const v = typeof otherMaybeOrFn === 'function' ? otherMaybeOrFn() : otherMaybeOrFn;
    if (!Maybe.isMaybe(v)) {
      throw new TypeError('orElse expects a Maybe or a function returning a Maybe');
    }
    return v;
  }

  equals(other, comparator) {
    if (!Maybe.isMaybe(other)) return false;
    if (this.isNothing() && other.isNothing()) return true;
    if (this.isJust() && other.isJust()) {
      if (comparator) return !!comparator(this.value, other.value);
      // Default: Object.is for primitives / reference equality for objects
      return Object.is(this.value, other.value);
    }
    return false;
  }

  toJSON() {
    return this.isNothing() ? { kind: 'nothing' } : { kind: 'just', value: this.value };
  }

  toString() {
    return this.isNothing() ? 'Nothing' : `Just(${String(this.value)})`;
  }
}

const NOTHING_SINGLETON = new Maybe('nothing');

export function Just(value) {
  return Maybe.Just(value);
}

export const Nothing = NOTHING_SINGLETON;

export function fromNullable(value) {
  return Maybe.fromNullable(value);
}
