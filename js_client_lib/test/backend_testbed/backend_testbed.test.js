// Vitest suite for JS Backend Testbed parity
import { describe, it, expect } from 'vitest';
import SimpleBackend from '../../interface/simple_backend.js';
import { BackendTestbed } from './backend_testbed.js';

describe('BackendTestbed parity suite', () => {
  it('add animals + notes + logical checks (empty prefix)', () => {
    const backend = new SimpleBackend();
    const tb = new BackendTestbed(backend, { shouldTestNotifications: false });
    // Idiom: addAnimalsToBackend, addNotesPageTree, then testBackendLogically("")
    tb.addAnimalsToBackend();
    tb.addNotesPageTree();
  // Should not throw; includes deletion of 'elephant'
  tb.testBackendLogically('');

  // Post-conditions: elephant deleted, lion & parrot remain
  expect(backend.getNode('lion').isJust()).toBe(true);
  expect(backend.getNode('elephant').isNothing()).toBe(true);
  expect(backend.getNode('parrot').isJust()).toBe(true);
  const notes = backend.getPageTree('notes');
  // After deletion, elephant notes (2 dossiers * 3) = 6 removed -> 12 remaining
  expect(notes.length).toBe(12);
  });
});
