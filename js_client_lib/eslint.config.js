// Flat ESLint config for ESLint v9+
// Mirrors the previous .eslintrc.json settings in flat format.
import js from '@eslint/js';

export default [
  js.configs.recommended,
  {
    languageOptions: {
      ecmaVersion: 2022,
      sourceType: 'module',
      globals: {
        // Vitest globals (tests)
        describe: 'readonly',
        it: 'readonly',
        expect: 'readonly',
  // Web APIs used in browser-safe code
  TextEncoder: 'readonly',
  TextDecoder: 'readonly',
      },
    },
    linterOptions: {
      reportUnusedDisableDirectives: true,
    },
    rules: {
      'no-unused-vars': ['warn', { argsIgnorePattern: '^_' }],
    },
    ignores: [
      'coverage/',
      'dist/',
      'build/',
      'node_modules/',
    ],
  },
];
