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
        // Common browser timers and fetch APIs (for browser-targeted files)
        setTimeout: 'readonly',
        clearTimeout: 'readonly',
        setInterval: 'readonly',
        clearInterval: 'readonly',
        fetch: 'readonly',
        Headers: 'readonly',
        AbortController: 'readonly',
        isSecureContext: 'readonly',
        console: 'readonly',
      },
    },
    linterOptions: {
  reportUnusedDisableDirectives: true,
    },
    rules: {
      'no-unused-vars': [
        'warn',
        {
          argsIgnorePattern: '^_',        // Ignore function params starting with _ (including '_')
          varsIgnorePattern: '^_$',       // Ignore local variables named exactly '_'
          caughtErrorsIgnorePattern: '^_$', // Ignore catch(e) when e is named exactly '_'
        },
      ],
      'no-empty': ['error', { allowEmptyCatch: true }],
    },
    ignores: [
      'coverage/',
      'dist/',
      'build/',
      'node_modules/',
    ],
  },
  // Node-specific files (transports and helpers)
  {
    files: [
      'transport/curl_communication.js',
      'transport/libcurl_transport.js',
      'transport/native_quic.js',
      'transport/node_*.js',
      'transport/create_webtransport_connector.js',
      'test/**',
    ],
    languageOptions: {
      globals: {
        process: 'readonly',
        Buffer: 'readonly',
        __dirname: 'readonly',
        setTimeout: 'readonly',
        clearTimeout: 'readonly',
        setInterval: 'readonly',
        clearInterval: 'readonly',
        console: 'readonly',
        AbortController: 'readonly',
      },
    },
  },
  // Ignore incomplete browser WebTransport adapter for now
  {
    ignores: [
      'transport/webtransport_communication.js',
    ],
  },
];
