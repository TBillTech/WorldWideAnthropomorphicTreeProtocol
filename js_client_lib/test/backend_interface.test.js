import { describe, it, expect } from 'vitest';
import { Backend, Notification, SequentialNotification, Nothing, Just } from '../index.js';

class TestBackend extends Backend {}

describe('Backend interface', () => {
  it('abstract methods throw by default', () => {
    const b = new TestBackend();
    expect(() => b.getNode('a')).toThrowError();
    expect(() => b.upsertNode([])).toThrowError();
    expect(() => b.deleteNode('a')).toThrowError();
    expect(() => b.getPageTree('p')).toThrowError();
    expect(() => b.relativeGetPageTree({}, 'p')).toThrowError();
    expect(() => b.queryNodes('q')).toThrowError();
    expect(() => b.relativeQueryNodes({}, 'q')).toThrowError();
    expect(() => b.openTransactionLayer({})).toThrowError();
    expect(() => b.closeTransactionLayers()).toThrowError();
    expect(() => b.applyTransaction([])).toThrowError();
    expect(() => b.getFullTree()).toThrowError();
    expect(() => b.registerNodeListener('n','l', false, () => {})).toThrowError();
    expect(() => b.deregisterNodeListener('n','l')).toThrowError();
    expect(() => b.notifyListeners('l', Nothing)).toThrowError();
    expect(() => b.processNotifications()).toThrowError();
  });

  it('Notification shapes', () => {
    const n1 = Notification('/lion', Nothing);
    expect(n1.labelRule).toBe('/lion');
    expect(n1.maybeNode.isNothing()).toBe(true);

    const n2 = Notification('/lion/pup', Just({ labelRule: '/lion/pup' }));
    expect(n2.maybeNode.isJust()).toBe(true);

    const sn = SequentialNotification(3, n2);
    expect(sn.signalCount).toBe(3);
    expect(sn.notification.labelRule).toBe('/lion/pup');
  });
});
