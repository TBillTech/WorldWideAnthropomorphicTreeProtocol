// Backend abstract interface and notification shapes for browser-safe usage.
// Methods mirror the C++ Backend interface. Implementations should extend Backend and override methods.
// Keep this file dependency-free except for Maybe helpers.

import { Maybe, Nothing } from './maybe.js';

export class Backend {
	// Optionally accept config in concrete implementations
	constructor() {}

	// Return Maybe<TreeNode>
	getNode(_labelRule) { throw new Error('getNode() not implemented'); }

	// Upsert multiple nodes, return boolean for success
	upsertNode(_nodes) { throw new Error('upsertNode() not implemented'); }

	deleteNode(_labelRule) { throw new Error('deleteNode() not implemented'); }

	getPageTree(_pageNodeLabelRule) { throw new Error('getPageTree() not implemented'); }

	relativeGetPageTree(_node, _pageNodeLabelRule) { throw new Error('relativeGetPageTree() not implemented'); }

	queryNodes(_labelRule) { throw new Error('queryNodes() not implemented'); }

	relativeQueryNodes(_node, _labelRule) { throw new Error('relativeQueryNodes() not implemented'); }

	openTransactionLayer(_node) { throw new Error('openTransactionLayer() not implemented'); }

	closeTransactionLayers() { throw new Error('closeTransactionLayers() not implemented'); }

	applyTransaction(_tx) { throw new Error('applyTransaction() not implemented'); }

	getFullTree() { throw new Error('getFullTree() not implemented'); }

	registerNodeListener(_listenerName, _labelRule, _childNotify, _cb) { throw new Error('registerNodeListener() not implemented'); }

	deregisterNodeListener(_listenerName, _labelRule) { throw new Error('deregisterNodeListener() not implemented'); }

	notifyListeners(_labelRule, _maybeNode) { throw new Error('notifyListeners() not implemented'); }

	processNotifications() { throw new Error('processNotifications() not implemented'); }
}

// Notification shapes
// Notification: { labelRule: string, maybeNode: Maybe<TreeNode> }
export function Notification(labelRule, maybeNode = Nothing) {
	return { labelRule, maybeNode };
}

// SequentialNotification: { signalCount: number, notification: Notification }
export function SequentialNotification(signalCount, notification) {
	return { signalCount, notification };
}

export { Maybe, Nothing };
