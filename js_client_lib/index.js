// Public entry for the JS client lib. Keep runtime APIs browser-safe.
// Re-export interfaces as they are implemented incrementally.
export { default as Communication } from './transport/communication.js';
export { default as StreamIdentifier } from './transport/stream_identifier.js';
export { default as Request } from './transport/request.js';
export { default as FetchCommunication } from './transport/fetch_communication.js';
export { default as WebTransportCommunication } from './transport/webtransport_communication.js';
export { default as MockCommunication } from './transport/mock_communication.js';
export { Maybe, Just, Nothing, fromNullable } from './interface/maybe.js';
export { Backend, Notification, SequentialNotification } from './interface/backend.js';
export {
	TreeNodeVersion,
	TreeNode,
	fixedSizeTypes,
	fromYAMLNode,
	toYAML,
	prefixNewNodeVersionLabels,
	shortenNewNodeVersionLabels,
	prefixSubTransactionLabels,
	shortenSubTransactionLabels,
	prefixTransactionLabels,
	shortenTransactionLabels,
} from './interface/tree_node.js';
// export * from './interface/backend.js'; // to be implemented
export { default as SimpleBackend } from './interface/simple_backend.js';
export * as Http3Helpers from './interface/http3_tree_message_helpers.js';
export { default as HTTP3TreeMessage } from './interface/http3_tree_message.js';
export { default as Http3ClientBackend } from './http3_client.js';
