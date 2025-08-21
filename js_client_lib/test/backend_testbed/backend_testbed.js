// JS Backend Testbed helpers for WWATP parity suite
// Ported from C++ backend_testbed.h
import { TreeNode, TreeNodeVersion } from '../../interface/tree_node.js';
import { Just, Nothing } from '../../interface/maybe.js';

// Helper: create a TreeNode with no content
export function createNoContentTreeNode(labelRule, description, propertyInfos, version, childNames, queryHowTo = Nothing, qaSequence = Nothing) {
  return new TreeNode({
    labelRule,
    description,
    propertyInfos,
    version: version instanceof TreeNodeVersion ? version : new TreeNodeVersion(version || {}),
    childNames,
    propertyData: new Uint8Array(0),
    queryHowTo,
    qaSequence,
  });
}

// Helper: create an "animal" node with property data
export function createAnimalNode(animal, description, propertyInfos, version, childNames, propertyDataStrings, queryHowTo, qaSequence) {
  // propertyDataStrings: array of string values for each property
  // propertyInfos: [{type, name}]
  let propertyData = buildPropertyData(propertyInfos, propertyDataStrings);
  return new TreeNode({
    labelRule: animal,
    description,
    propertyInfos,
    version: version instanceof TreeNodeVersion ? version : new TreeNodeVersion(version || {}),
    childNames,
    propertyData,
    // Treat empty strings as Nothing to match chunk-based decode semantics
    queryHowTo: (typeof queryHowTo === 'string' && queryHowTo.length === 0) ? Nothing : Just(queryHowTo),
    qaSequence: (typeof qaSequence === 'string' && qaSequence.length === 0) ? Nothing : Just(qaSequence),
  });
}

function buildPropertyData(propertyInfos, propertyDataStrings) {
  // Builds Uint8Array for propertyData from propertyInfos and propertyDataStrings
  // Only supports uint64 and string for now (as in C++ testbed)
  let totalLen = 0;
  let parts = [];
  for (let i = 0; i < propertyInfos.length; i++) {
    const { type } = propertyInfos[i];
    const value = propertyDataStrings[i];
    if (type === 'uint64') {
      // 8 bytes, little-endian
      const buf = new ArrayBuffer(8);
      const dv = new DataView(buf);
      dv.setBigUint64(0, BigInt(value), true);
      parts.push(new Uint8Array(buf));
      totalLen += 8;
    } else if (type === 'string') {
      // 4-byte length prefix + UTF-8 bytes
      const strBytes = new TextEncoder().encode(value);
      const lenBuf = new ArrayBuffer(4);
      new DataView(lenBuf).setUint32(0, strBytes.length, true);
      parts.push(new Uint8Array(lenBuf));
      parts.push(strBytes);
      totalLen += 4 + strBytes.length;
    }
    // Extend for other types as needed
  }
  let out = new Uint8Array(totalLen);
  let offset = 0;
  for (let p of parts) {
    out.set(p, offset);
    offset += p.length;
  }
  return out;
}

export function createAnimalDossiers(animalNode) {
  // For each child, create a dossier node and 3 note nodes
  let animal_dossiers = [];
  for (const child_name of animalNode.getChildNames()) {
    const child_label_rule = animalNode.getLabelRule() + '/' + child_name;
    const child_version = new TreeNodeVersion({ versionNumber: 1, maxVersionSequence: 256, policy: 'public', collisionDepth: Just(2) });
    const child_node = createNoContentTreeNode(child_label_rule, 'Animal Dossier', [], child_version, [], Nothing, Nothing);
    let note_names = [];
    for (let i = 0; i < 3; ++i) {
      const note_label_rule = child_label_rule + '/note' + i;
      note_names.push('note' + i);
      const note_version = new TreeNodeVersion({ versionNumber: 1, maxVersionSequence: 256, policy: 'public', collisionDepth: Just(2) });
      const note_node = createNoContentTreeNode(note_label_rule, child_name + ' note ' + i, [], note_version, [], Nothing, Nothing);
      animal_dossiers.push(note_node);
    }
    child_node.setChildNames(note_names);
    animal_dossiers.push(child_node);
  }
  return animal_dossiers;
}

export function createLionNodes() {
  const lion = createAnimalNode(
    'lion',
    'King of the jungle',
    [{ type: 'uint64', name: 'popularity' }, { type: 'string', name: 'diet' }],
    new TreeNodeVersion({ versionNumber: 1, maxVersionSequence: 256, policy: 'public', authors: Just('tester'), readers: Just('tester'), collisionDepth: Just(2) }),
    ['Simba', 'Nala'],
    ['10', 'carnivore'],
    'url duh!',
    'Zookeeper1: Lions are majestic.\nZookeeper2: Indeed, they are the kings of the jungle.'
  );
  let lion_nodes = createAnimalDossiers(lion);
  lion_nodes.unshift(lion);
  return lion_nodes;
}

export function createElephantNodes() {
  const elephant = createAnimalNode(
    'elephant',
    'Largest land animal',
    [{ type: 'uint64', name: 'popularity' }, { type: 'string', name: 'diet' }],
    new TreeNodeVersion({ versionNumber: 1, maxVersionSequence: 256, policy: 'public', collisionDepth: Just(2) }),
    ['Dumbo', 'Babar'],
    ['8', 'herbivore'],
    'url duh!',
    'Zookeeper1: Elephants are so strong.\nZookeeper2: And they have great memory!'
  );
  let elephant_nodes = createAnimalDossiers(elephant);
  elephant_nodes.unshift(elephant);
  return elephant_nodes;
}

export function createParrotNodes() {
  const parrot = createAnimalNode(
    'parrot',
    'Colorful bird',
    [{ type: 'uint64', name: 'popularity' }, { type: 'string', name: 'diet' }],
    new TreeNodeVersion({ versionNumber: 1, maxVersionSequence: 256, policy: 'public', collisionDepth: Just(2) }),
    ['Polly', 'Jerome'],
    ['7', 'omnivore'],
    'url duh!',
    'Zookeeper1: Parrots can mimic sounds.\nZookeeper2: Yes, they are very intelligent birds.'
  );
  let parrot_nodes = createAnimalDossiers(parrot);
  parrot_nodes.unshift(parrot);
  return parrot_nodes;
}

export function collectAllNotes() {
  const lion_nodes = createLionNodes();
  const elephant_nodes = createElephantNodes();
  const parrot_nodes = createParrotNodes();
  let all_notes = [];
  for (const node of [...lion_nodes, ...elephant_nodes, ...parrot_nodes]) {
    if (node.getLabelRule().includes('note')) {
      all_notes.push(node);
    }
  }
  return all_notes;
}

export function createNotesPageTree() {
  const noteNodes = collectAllNotes();
  const note_labels = noteNodes.map((n) => n.getLabelRule());
  // page_nodes property: variable-size, name 'page_nodes', value is JSON array of label rules
  const page_nodes_info = [{ type: 'string', name: 'page_nodes' }];
  const page_nodes_data = [JSON.stringify(note_labels)];
  const node = createNoContentTreeNode(
    'notes',
    'Animal Notes Page',
    page_nodes_info,
    new TreeNodeVersion({ versionNumber: 1, maxVersionSequence: 256, policy: 'public', collisionDepth: Just(2) }),
    note_labels,
    Nothing,
    Nothing
  );
  node.setPropertyData(buildPropertyData(page_nodes_info, page_nodes_data));
  return node;
}

export function prefixNodeLabels(labelPrefix, nodes) {
  return nodes.map((node) => {
    const n = node.clone();
    if (labelPrefix) {
      n.setLabelRule(labelPrefix + n.getLabelRule());
      n.setChildNames(n.getChildNames().map((c) => labelPrefix + c));
    }
    return n;
  });
}

// BackendTestbed class
export class BackendTestbed {
  constructor(backend, opts = {}) {
    this.backend = backend;
    this.shouldTestNotifications = opts.shouldTestNotifications ?? true;
    this.shouldTestChanges = opts.shouldTestChanges ?? true;
  }

  addAnimalsToBackend() {
    this.backend.upsertNode(createLionNodes());
    this.backend.upsertNode(createElephantNodes());
    this.backend.upsertNode(createParrotNodes());
  }

  addNotesPageTree() {
    this.backend.upsertNode([createNotesPageTree()]);
  }

  testBackendLogically(labelPrefix = '') {
    // Check all animal nodes exist
    checkMultipleGetNode(this.backend, prefixNodeLabels(labelPrefix, createLionNodes()));
    checkMultipleGetNode(this.backend, prefixNodeLabels(labelPrefix, createElephantNodes()));
    checkMultipleGetNode(this.backend, prefixNodeLabels(labelPrefix, createParrotNodes()));

    // Check notes page tree
    const notesPages = this.backend.getPageTree(labelPrefix + 'notes');
    const expectedNotes = prefixNodeLabels(labelPrefix, collectAllNotes());
    if (notesPages.length !== expectedNotes.length || !notesPages.every((n, i) => n.equals(expectedNotes[i]))) {
      throw new Error('Notes page tree does not match expected notes');
    }

    // Full tree round-trip
  this.backend.getFullTree();
    // For round-trip, create a new backend and upsert all nodes
    // (Assume SimpleBackend is available in test file)

    // Test changes: delete elephant, check
    if (this.shouldTestChanges) {
      const label_rule = labelPrefix + 'elephant';
      this.backend.deleteNode(label_rule);
      checkMultipleDeletedNode(this.backend, prefixNodeLabels(labelPrefix, createElephantNodes()));
      checkMultipleGetNode(this.backend, prefixNodeLabels(labelPrefix, createLionNodes()));
      checkMultipleGetNode(this.backend, prefixNodeLabels(labelPrefix, createParrotNodes()));
    }
    // Notification tests are handled in test file for now
  }

  // Port of C++ BackendTestbed::testPeerNotification
  // toBeModified: Backend performing the external mutations (peer)
  // notificationDelay: ms to wait between peer action and local processNotifications
  // labelPrefix: optional prefix for labels
  async testPeerNotification(toBeModified, notificationDelay = 0, labelPrefix = '') {
    if (!this.shouldTestNotifications || !this.shouldTestChanges) return;

    const sleep = (ms) => new Promise((r) => setTimeout(r, Math.max(0, Number(ms) | 0)));
    const awaitIfPromise = async (v) => (v && typeof v.then === 'function' ? await v : v);

    let lionCreated = false;
    let lionDeleted = false;
    const lionLabel = String(labelPrefix || '') + 'lion';

    // Ensure clean state on the peer backend
    await awaitIfPromise(toBeModified.deleteNode(lionLabel));

    // Register listener on our local backend
    const selfBackend = this.backend;
    await awaitIfPromise(this.backend.registerNodeListener('lion_listener', lionLabel, false, (notifiedBackend, labelRule, maybeNode) => {
      if (labelRule !== lionLabel) throw new Error(`Listener label mismatch: got ${labelRule}, expected ${lionLabel}`);
      if (notifiedBackend !== selfBackend) throw new Error('Listener backend instance mismatch');
      if (maybeNode && typeof maybeNode.isJust === 'function' && maybeNode.isJust()) {
        const node = maybeNode.getOrElse(null);
        if (node && typeof node.getLabelRule === 'function' && node.getLabelRule() === lionLabel) {
          lionCreated = true;
        }
      } else {
        // Nothing => deletion
        lionDeleted = true;
      }
    }));

    const prefixedLionNodes = prefixNodeLabels(labelPrefix || '', createLionNodes());

    // Create on the peer
    await awaitIfPromise(toBeModified.upsertNode(prefixedLionNodes));
    await awaitIfPromise(toBeModified.processNotifications());
    await sleep(notificationDelay);
    await awaitIfPromise(this.backend.processNotifications());
    if (!lionCreated) throw new Error('Expected lion node creation notification');

    // Now delete on the peer
    await awaitIfPromise(toBeModified.deleteNode(lionLabel));
    await awaitIfPromise(toBeModified.processNotifications());
    await sleep(notificationDelay);
    await awaitIfPromise(this.backend.processNotifications());
    if (!lionDeleted) throw new Error('Expected lion node deletion notification');

    // Deregister and ensure no further notifications are observed
    await awaitIfPromise(this.backend.deregisterNodeListener('lion_listener', lionLabel));
    await awaitIfPromise(toBeModified.processNotifications());
    await sleep(notificationDelay);
    await awaitIfPromise(this.backend.processNotifications());

    lionCreated = false;
    lionDeleted = false;

    await awaitIfPromise(toBeModified.upsertNode(prefixedLionNodes));
    await awaitIfPromise(toBeModified.processNotifications());
    await sleep(notificationDelay);
    await awaitIfPromise(this.backend.processNotifications());
    if (lionCreated) throw new Error('Listener should be deregistered; unexpected creation notification');

    await awaitIfPromise(toBeModified.deleteNode(lionLabel));
    await awaitIfPromise(toBeModified.processNotifications());
    await sleep(notificationDelay);
    await awaitIfPromise(this.backend.processNotifications());
    if (lionDeleted) throw new Error('Listener should be deregistered; unexpected deletion notification');
  }
}

// Test helpers
export function checkGetNode(backend, labelRule, expectedNode) {
  const maybeNode = backend.getNode(labelRule);
  if (maybeNode.isJust()) {
    const foundNode = maybeNode.getOrElse(null);
    if (!foundNode.equals(expectedNode)) {
      throw new Error(`Node mismatch for ${labelRule}`);
    }
  } else {
    throw new Error(`Node not found: ${labelRule}`);
  }
}

export function checkMultipleGetNode(backend, expectedNodes) {
  for (const expectedNode of expectedNodes) {
    checkGetNode(backend, expectedNode.getLabelRule(), expectedNode);
  }
}

export function checkDeletedNode(backend, labelRule) {
  const maybeNode = backend.getNode(labelRule);
  if (!maybeNode.isNothing()) {
    throw new Error(`Node should be deleted: ${labelRule}`);
  }
}

export function checkMultipleDeletedNode(backend, expectedNodes) {
  for (const expectedNode of expectedNodes) {
    checkDeletedNode(backend, expectedNode.getLabelRule());
  }
}
