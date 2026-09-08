import { performance } from "node:perf_hooks";
import { ConnectionError, type LiveMixConnection } from "./connection.js";
import { sendTarget, type Ack, type EditCommand, type Snapshot } from "./protocol.js";

type Contribution = { owner: string; ticks: number; step: number; resolve: () => void; reject: (error: ConnectionError) => void };
type Intent = {
  parts: Contribution[]; epoch: number; expires: number; notBefore: number;
  compute: (snapshot: Snapshot) => EditCommand;
  computed: boolean; retries: number; retrying: boolean;
  rotation?: { channelId: string; fxId: string; last: number };
  expiryTimer?: ReturnType<typeof setTimeout>;
};
type TargetQueue = { waiting: Intent[]; active?: Intent; ack?: Ack; timer?: ReturnType<typeof setTimeout>; wake?: ReturnType<typeof setTimeout> };

/** One queue for every action/context. Values are computed only from canonical state at dispatch. */
export class CommandQueue {
  private epoch = 0;
  private readonly targets = new Map<string, TargetQueue>();
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
  enqueue(target: string, owner: string, compute: Intent["compute"], computed = false, retries = 0): Promise<void> {
    if (!this.connection.ready) return Promise.reject(new ConnectionError("DISCONNECTED"));
    const queue = this.target(target);
    const keys = queue.waiting.filter(i => !i.rotation);
    if (keys.length >= 2) {
      if (!computed) return Promise.reject(new ConnectionError("INPUT_BUSY"));
      // Computed keys keep the two newest intentions, visibly failing the displaced one.
      queue.waiting.splice(queue.waiting.indexOf(keys[0]!), 1); this.reject(keys[0]!, "INPUT_BUSY");
    }
    return new Promise((resolve, reject) => {
      const now = performance.now();
      this.push(target, queue, { parts: [{ owner, ticks: 0, step: 0, resolve, reject }], epoch: this.epoch,
        expires: now + 500, notBefore: now, compute, computed, retries, retrying: false });
    });
  }
  rotate(target: string, owner: string, channelId: string, fxId: string, ticks: number, step: number): Promise<void> {
    if (!this.connection.ready) return Promise.reject(new ConnectionError("DISCONNECTED"));
    if (!Number.isSafeInteger(ticks) || ![1, 5].includes(step)) return Promise.reject(new ConnectionError("INVALID_SETTINGS"));
    if (!ticks) return Promise.resolve();
    const queue = this.target(target), now = performance.now();
    const rotations = queue.waiting.filter(i => i.rotation);
    const unsent = rotations.reduce((sum, i) => sum + i.parts.reduce((n, p) => n + Math.abs(p.ticks), 0), 0);
    if (unsent + Math.abs(ticks) > 50 || rotations.some(i => i.expires <= now)) {
      queue.waiting = queue.waiting.filter(i => { if (!i.rotation) return true; this.reject(i, "INPUT_BUSY"); return false; });
      this.advance(target, queue);
      return Promise.reject(new ConnectionError("INPUT_BUSY"));
    }
    return new Promise((resolve, reject) => {
      const part = { owner, ticks, step, resolve, reject }, tail = queue.waiting.at(-1);
      if (tail?.rotation && now - tail.rotation.last <= 50) {
        tail.parts.push(part); tail.rotation.last = now; tail.notBefore = now + 50;
        this.advance(target, queue); return;
      }
      const intent: Intent = { parts: [part], epoch: this.epoch, expires: now + 500, notBefore: now + 50,
        computed: true, retries: 2, retrying: false, rotation: { channelId, fxId, last: now },
        compute: snapshot => {
          const current = sendTarget(snapshot, channelId, fxId);
          if (!current) throw new ConnectionError("SEND_NOT_FOUND");
          const change = intent.parts.reduce((sum, p) => sum + p.ticks * p.step / 100, 0);
          const amount = Math.min(1, Math.max(0, current.send.amount + change));
          return { command: "setSend", args: { channelId, fxId, amount } };
        } };
      this.push(target, queue, intent);
    });
  }
  cancelOwner(owner: string): void {
    for (const [target, queue] of this.targets) {
      for (const intent of [...queue.waiting, ...(queue.active ? [queue.active] : [])]) {
        intent.parts = intent.parts.filter(p => { if (p.owner !== owner) return true; p.reject(new ConnectionError("STALE_INPUT")); return false; });
      }
      queue.waiting = queue.waiting.filter(i => { if (i.parts.length) return true; clearTimeout(i.expiryTimer); return false; });
      this.advance(target, queue);
    }
  }
  private target(target: string): TargetQueue {
    let queue = this.targets.get(target);
    if (!queue) { queue = { waiting: [] }; this.targets.set(target, queue); }
    return queue;
  }
  private push(target: string, queue: TargetQueue, intent: Intent): void {
    intent.expiryTimer = setTimeout(() => {
      const index = queue.waiting.indexOf(intent);
      if (index >= 0) { queue.waiting.splice(index, 1); this.reject(intent, "INPUT_EXPIRED"); this.advance(target, queue); }
    }, Math.max(1, intent.expires - performance.now()));
    queue.waiting.push(intent); this.advance(target, queue);
  }
  private reject(intent: Intent, code: string): void {
    clearTimeout(intent.expiryTimer); for (const p of intent.parts) p.reject(new ConnectionError(code));
  }
  private clear(code: string): void {
    this.epoch++;
    for (const [target, queue] of this.targets) this.failTarget(target, queue, code);
  }
  private advance(target: string, queue: TargetQueue): void {
    if (this.targets.get(target) !== queue) return;
    clearTimeout(queue.wake); queue.wake = undefined;
    const snapshot = this.connection.store.snapshot;
    if (!snapshot || !this.connection.ready) return;
    if (queue.active && queue.ack && snapshot.instanceId === queue.ack.instanceId && snapshot.sessionId === queue.ack.sessionId && snapshot.revision >= queue.ack.revision) {
      clearTimeout(queue.timer); for (const p of queue.active.parts) p.resolve(); queue.active = undefined; queue.ack = undefined;
    }
    if (queue.active) {
      if (queue.active.retrying) {
        if (!queue.active.parts.length || queue.active.expires < performance.now()) { this.failTarget(target, queue, "INPUT_EXPIRED"); return; }
        this.dispatch(target, queue, snapshot);
      }
      return;
    }
    const now = performance.now();
    while (queue.waiting[0] && (queue.waiting[0].epoch !== this.epoch || queue.waiting[0].expires <= now)) this.reject(queue.waiting.shift()!, "INPUT_EXPIRED");
    const intent = queue.waiting[0];
    if (!intent) { this.targets.delete(target); return; }
    if (intent.notBefore > now) {
      queue.wake = setTimeout(() => this.advance(target, queue), Math.max(1, intent.notBefore - now)); return;
    }
    queue.active = queue.waiting.shift()!; clearTimeout(intent.expiryTimer);
    this.dispatch(target, queue, snapshot);
  }
  private dispatch(target: string, queue: TargetQueue, snapshot: Snapshot): void {
    const active = queue.active!; active.retrying = false; clearTimeout(queue.timer);
    let command: EditCommand;
    try { command = active.compute(snapshot); }
    catch (error) { this.failTarget(target, queue, error instanceof ConnectionError ? error.code : "COMMAND_FAILED"); return; }
    void this.connection.command(command, active.computed ? snapshot.revision : undefined).then(ack => {
      if (active.epoch !== this.epoch || this.targets.get(target) !== queue) return;
      queue.ack = ack;
      queue.timer = setTimeout(() => { this.failTarget(target, queue, "STATE_TIMEOUT"); this.connection.requestState(); }, 2000);
      this.advance(target, queue);
    }).catch((error: unknown) => {
      if (this.targets.get(target) !== queue) return;
      const code = error instanceof ConnectionError ? error.code : "COMMAND_FAILED";
      if (code === "REVISION_CONFLICT" && active.retries > 0 && active.parts.length) {
        active.retries--; active.retrying = true;
        // Connection's conflict path requests a snapshot before this continuation runs.
        queue.timer = setTimeout(() => this.failTarget(target, queue, "INPUT_EXPIRED"), Math.max(1, active.expires - performance.now()));
        this.advance(target, queue);
      } else this.failTarget(target, queue, code);
    });
  }
  private failTarget(target: string, queue: TargetQueue, code: string): void {
    clearTimeout(queue.timer); clearTimeout(queue.wake);
    if (queue.active) this.reject(queue.active, code);
    for (const intent of queue.waiting) this.reject(intent, "STALE_INPUT");
    this.targets.delete(target);
  }
}
