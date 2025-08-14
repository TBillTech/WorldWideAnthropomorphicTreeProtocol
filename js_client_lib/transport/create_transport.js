// Factory for selecting a browser-friendly transport based on feature detection.
// - If WebTransport is available and allowed (secure context), returns WebTransportCommunication
// - Else falls back to FetchCommunication (non-streaming)
// Consumers can also pass override via options.preferred: 'webtransport' | 'fetch'

import WebTransportCommunication from './webtransport_communication.js';
import FetchCommunication from './fetch_communication.js';

export function isSecureContextLike() {
  // In browsers, window.isSecureContext; in Node/tests, assume true
  try { return typeof isSecureContext !== 'undefined' ? !!isSecureContext : true; } catch { return true; }
}

export function hasWebTransport() {
  try { return typeof WebTransport !== 'undefined'; } catch { return false; }
}

export function createTransportForUrl(url, options = {}) {
  const pref = options.preferred;
  const canWT = hasWebTransport() && isSecureContextLike();
  if ((pref === 'webtransport' || !pref) && canWT) {
    return new WebTransportCommunication(url);
  }
  // Fallback: Fetch
  const base = typeof url === 'string' ? url.replace(/\/$/, '') : String(url || '');
  return new FetchCommunication(base);
}

export default createTransportForUrl;
