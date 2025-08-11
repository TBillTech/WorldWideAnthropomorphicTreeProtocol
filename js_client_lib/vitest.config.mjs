// Vitest v3 config to emulate the deprecated 'basic' reporter
// Recommendation from Vitest deprecation message:
// reporters: [["default", { summary: false }]]
import { defineConfig } from 'vitest/config';

export default defineConfig({
  test: {
    reporters: [["default", { summary: false }]],
    coverage: {
      provider: 'v8'
    }
  }
});
