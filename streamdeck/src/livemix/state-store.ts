import { EventEmitter } from "node:events";
import { validateProjection, type Delta, type Snapshot } from "./protocol.js";

export class StateStore extends EventEmitter {
  snapshot: Snapshot | undefined;
  synced = false;
  reset(): void {
    this.snapshot = undefined; this.synced = false;
    this.emit("boundary"); this.emit("change");
  }
  invalidate(): false {
    if (this.synced) { this.synced = false; this.emit("gap"); this.emit("change"); }
    return false;
  }
  accept(message: Snapshot | Delta): boolean {
    const old = this.snapshot;
    if (message.type === "state") {
      if (old && old.instanceId === message.instanceId && message.revision < old.revision) return this.invalidate();
      if (old && (old.instanceId !== message.instanceId || old.sessionId !== message.sessionId)) this.emit("boundary");
      this.snapshot = message; this.synced = true; this.emit("change"); return true;
    }
    if (!old || !this.synced || old.instanceId !== message.instanceId || old.sessionId !== message.sessionId
      || old.revision !== message.baseRevision || message.revision <= message.baseRevision) return this.invalidate();
    const c = message.changes, s = old.state;
    // An unknown ID is a gap, never an implicit insert. Array order comes only from full state.
    if (c.channels?.some(p => !s.channels.some(x => x.id === p.id)) || c.fx?.some(p => !s.fx.some(x => x.id === p.id))) return this.invalidate();
    try {
      const state = validateProjection({
        session: { ...s.session, ...c.session }, audio: { ...s.audio, ...c.audio }, muteGroups: { ...s.muteGroups, ...c.muteGroups },
        channels: s.channels.map(x => ({ ...x, ...c.channels?.find(p => p.id === x.id) })),
        fx: s.fx.map(x => ({ ...x, ...c.fx?.find(p => p.id === x.id) }))
      });
      this.snapshot = { ...old, revision: message.revision, state };
      this.emit("change"); return true;
    } catch { return this.invalidate(); }
  }
}
