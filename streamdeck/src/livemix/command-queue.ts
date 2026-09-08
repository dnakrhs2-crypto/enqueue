import { performance } from "node:perf_hooks";
import { ConnectionError, type LiveMixConnection } from "./connection.js";
import type { Ack, MicCommand, Snapshot } from "./protocol.js";

type Intent = {
  owner: string; epoch: number; expires: number;
  compute: (snapshot: Snapshot) => MicCommand;
  computed: boolean;
  expiryTimer?: ReturnType<typeof setTimeout>;
  resolve: () => void; reject: (error: ConnectionError) => void;
};
type TargetQueue = { waiting: Intent[]; active?: Intent; ack?: Ack; timer?: ReturnType<typeof setTimeout> };

/** One shared queue across all keys. Intents are evaluated only at dispatch against canonical state. */
export class CommandQueue {
  private epoch = 0;
  private targets = new Map<string, TargetQueue>();
  private readonly boundary = (): void => this.clear("STALE_INPUT");
  private readonly changed = (): void => { for (const [target, queue] of this.targets) this.advance(target, queue); };
  constructor(private readonly connection: LiveMixConnection) {
    connection.store.on("boundary", this.boundary);
    connection.store.on("change", this.changed);
    connection.on("status", this.changed);
  }
  dispose(): void {
    this.clear("STOPPED"); this.connection.store.off("boundary", this.boundary);
    this.connection.store.off("change", this.changed); this.connection.off("status", this.changed);
  }
  enqueue(target: string, owner: string, compute: Intent["compute"], computed = false): Promise<void> {
    if (!this.connection.ready) return Promise.reject(new ConnectionError("DISCONNECTED"));
    let queue = this.targets.get(target);
    if (!queue) { queue = { waiting: [] }; this.targets.set(target, queue); }
    if (queue.waiting.length >= 2) return Promise.reject(new ConnectionError("INPUT_BUSY"));
    const current = queue;
    return new Promise((resolve, reject) => {
      const intent: Intent = { owner, epoch: this.epoch, expires: performance.now() + 500, compute, computed, resolve, reject };
      intent.expiryTimer = setTimeout(() => {
        const index = current.waiting.indexOf(intent);
        if (index >= 0) { current.waiting.splice(index, 1); intent.reject(new ConnectionError("INPUT_EXPIRED")); }
      }, 500);
      current.waiting.push(intent);
      this.advance(target, current);
    });
  }
  cancelOwner(owner: string): void {
    for (const queue of this.targets.values()) {
      queue.waiting = queue.waiting.filter(intent => { if (intent.owner !== owner) return true; clearTimeout(intent.expiryTimer); intent.reject(new ConnectionError("STALE_INPUT")); return false; });
    }
  }
  private clear(code: string): void {
    this.epoch++;
    for (const q of this.targets.values()) {
      clearTimeout(q.timer); q.active?.reject(new ConnectionError(code));
      for (const intent of q.waiting) { clearTimeout(intent.expiryTimer); intent.reject(new ConnectionError(code)); }
    }
    this.targets.clear();
  }
  private advance(target: string, queue: TargetQueue): void {
    if (this.targets.get(target) !== queue) return;
    const snapshot = this.connection.store.snapshot;
    if (!snapshot || !this.connection.ready) return;
    if (queue.active && queue.ack && snapshot.instanceId === queue.ack.instanceId && snapshot.sessionId === queue.ack.sessionId && snapshot.revision >= queue.ack.revision) {
      clearTimeout(queue.timer); queue.active.resolve(); queue.active = undefined; queue.ack = undefined;
    }
    if (queue.active) return;
    let intent: Intent | undefined;
    while ((intent = queue.waiting.shift())) {
      clearTimeout(intent.expiryTimer);
      if (intent.epoch === this.epoch && intent.expires >= performance.now()) break;
      intent.reject(new ConnectionError("INPUT_EXPIRED")); intent = undefined;
    }
    if (!intent) { this.targets.delete(target); return; }
    queue.active = intent;
    const active = intent;
    let command: MicCommand;
    try { command = active.compute(snapshot); }
    catch { this.failTarget(target, queue, "CHANNEL_NOT_FOUND"); return; }
    void this.connection.command(command, active.computed ? snapshot.revision : undefined).then(ack => {
      if (active.epoch !== this.epoch || this.targets.get(target) !== queue) return;
      queue.ack = ack;
      // An ack without its state must not leave queued input blocked forever.
      queue.timer = setTimeout(() => { this.failTarget(target, queue, "STATE_TIMEOUT"); this.connection.requestState(); }, 2000);
      this.advance(target, queue);
    }).catch((error: unknown) => {
      if (this.targets.get(target) === queue) this.failTarget(target, queue, error instanceof ConnectionError ? error.code : "COMMAND_FAILED");
    });
  }
  private failTarget(target: string, queue: TargetQueue, code: string): void {
    clearTimeout(queue.timer); queue.active?.reject(new ConnectionError(code));
    for (const intent of queue.waiting) { clearTimeout(intent.expiryTimer); intent.reject(new ConnectionError("STALE_INPUT")); }
    this.targets.delete(target);
  }
}
