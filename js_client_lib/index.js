// Public entry for the JS client lib. Keep runtime APIs browser-safe.
// Re-export interfaces as they are implemented incrementally.
export { default as Communication } from './transport/communication.js';
export { Maybe, Just, Nothing, fromNullable } from './interface/maybe.js';
export { Backend, Notification, SequentialNotification } from './interface/backend.js';
// export * from './interface/backend.js'; // to be implemented
