/* eslint-env node */
// Lightweight tracing helper gated by env/global flag.
// Enable by setting WWATP_TRACE=1 (or 'true', 'on') in env or globalThis.

function isEnabled() {
  try {
    const v = (typeof globalThis !== 'undefined' && globalThis.process && globalThis.process.env && globalThis.process.env.WWATP_TRACE) ||
              (typeof globalThis !== 'undefined' && globalThis.WWATP_TRACE);
    if (!v) return false;
    const s = String(v).toLowerCase();
    return s === '1' || s === 'true' || s === 'on' || s === 'yes' || s === 'debug';
  } catch {
    return false;
  }
}

const state = {
  counters: Object.create(null),
};

function fmtTs() {
  try { return new Date().toISOString(); } catch { return '';
  }
}

export function tracer(scope = 'trace') {
  const enabled = isEnabled();
  function inc(name, by = 1) {
    if (!enabled) return;
    const key = `${scope}.${name}`;
    state.counters[key] = (state.counters[key] | 0) + by;
  }
  function log(level, msg, details) {
    if (!enabled) return;
    const prefix = `[${fmtTs()}][${scope}]`;
    try {
      console[level](`${prefix} ${msg}`, details ?? '');
    } catch {}
  }
  return {
    enabled,
    inc,
    debug: (msg, details) => log('debug', msg, details),
    info: (msg, details) => log('info', msg, details),
    warn: (msg, details) => log('warn', msg, details),
    error: (msg, details) => log('error', msg, details),
  };
}

export function getTraceState() {
  return { counters: { ...state.counters } };
}
