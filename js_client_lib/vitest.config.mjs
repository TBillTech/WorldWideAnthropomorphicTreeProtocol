// Vitest v3 config to emulate the deprecated 'basic' reporter
// Recommendation from Vitest deprecation message:
// reporters: [["default", { summary: false }]]
import { defineConfig } from 'vitest/config';

// Removed custom sequencer; unique ports eliminate cross-file interference.

export default defineConfig({
  test: {
    reporters: [["default", { summary: false }]],
    // Run E2E in a single thread to avoid cross-file parallelism when requested
    poolOptions: {
      threads: {
        singleThread: ['1','true','yes','on'].includes(String(process.env.WWATP_E2E || '').toLowerCase())
      }
    },
    sequence: {
  shuffle: false
    },
    coverage: {
      provider: 'v8'
    }
  }
});
