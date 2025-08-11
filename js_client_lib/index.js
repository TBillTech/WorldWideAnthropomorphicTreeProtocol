// Public entry for the JS client lib. Keep runtime APIs browser-safe.
// Re-export interfaces as they are implemented incrementally.
export { default as Communication } from './transport/communication.js';
// export * from './interface/backend.js'; // to be implemented
