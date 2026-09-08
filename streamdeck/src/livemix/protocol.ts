// Wire authority: livemix/src/ControlProtocol.{h,cpp}, ControlServer.cpp.
export const VERSION = 1;
export const MAX_LINE_BYTES = 64 * 1024;
export type Json = null | boolean | number | string | Json[] | { [key: string]: Json };
export type ObjectValue = Record<string, unknown>;
export const isObject = (v: unknown): v is ObjectValue => typeof v === "object" && v !== null && !Array.isArray(v);
export const isUuid = (v: unknown): v is string => typeof v === "string" && /^[0-9a-f]{32}$/.test(v) && /[1-9a-f]/.test(v);
export const isRevision = (v: unknown): v is number => Number.isSafeInteger(v) && (v as number) >= 0;
export const isRequestId = (v: unknown): v is string => typeof v === "string" && /^[1-9][0-9]{0,15}$/.test(v) && Number(v) <= Number.MAX_SAFE_INTEGER;
export class ProtocolError extends Error {
  constructor(readonly code: string) { super(code); }
}
function requireValue(condition: unknown): asserts condition {
  if (!condition) throw new ProtocolError("INVALID_MESSAGE");
}
function object(v: unknown): ObjectValue { requireValue(isObject(v)); return v; }
function str(v: unknown): string { requireValue(typeof v === "string"); return v; }
function bool(v: unknown): boolean { requireValue(typeof v === "boolean"); return v; }
function uuid(v: unknown): string { requireValue(isUuid(v)); return v; }
function rev(v: unknown): number { requireValue(isRevision(v)); return v; }
function id(v: unknown): string { requireValue(isRequestId(v)); return v; }
function fraction(v: unknown): number { requireValue(typeof v === "number" && Number.isFinite(v) && v >= 0 && v <= 1); return v; }
function list<T>(v: unknown, max: number, read: (v: unknown) => T, key: (v: T) => unknown): T[] {
  requireValue(Array.isArray(v) && v.length <= max);
  const result = v.map(read);
  requireValue(new Set(result.map(key)).size === result.length);
  return result;
}
export type PluginGroup = { index: number; off: boolean };
export type Send = { fxId: string; amount: number; pre: boolean };
export type Channel = { id: string; name: string; on: boolean; muteGroup: boolean; pluginGroups: PluginGroup[]; sends: Send[] };
export type Fx = { id: string; name: string; muteGroup: boolean; return: number };
export type Projection = {
  session: { name: string; dirty: boolean };
  audio: { running: boolean };
  channels: Channel[];
  fx: Fx[];
  muteGroups: { mic: boolean; fx: boolean };
};
export type Changes = {
  session?: Partial<Projection["session"]>;
  audio?: Partial<Projection["audio"]>;
  muteGroups?: Partial<Projection["muteGroups"]>;
  channels?: (Partial<Channel> & { id: string })[];
  fx?: (Partial<Fx> & { id: string })[];
};
export type Context = { instanceId: string; sessionId: string; revision: number };
export type Snapshot = Context & { v: 1; type: "state"; reason: string; requestId?: string; state: Projection };
export type Delta = Context & { v: 1; type: "stateDelta"; baseRevision: number; changes: Changes };
export type Ack = Context & { v: 1; type: "ack"; id: string; changed: boolean; result: ObjectValue };
export type WireError = Partial<Context> & { v: 1; type: "error"; id: string | null; code: string; message: string; retryable: boolean };
export type HelloAck = { v: 1; type: "helloAck"; id: string; instanceId: string; server: { name: string; version: string }; capabilities: string[]; eventIntervalMs: number; heartbeatIntervalMs: number };
export type ServerMessage = Snapshot | Delta | Ack | WireError | HelloAck
  | (Context & { v: 1; type: "pong"; id: string })
  | { v: 1; type: "serverStatus"; instanceId: string; status: "disabled" | "stopping"; reason: string }
  | (Context & { v: 1; type: "unknown"; wireType: string });
export type MicCommand = { command: "toggleChannel"; args: { channelId: string } }
  | { command: "setChannelOn"; args: { channelId: string; on: boolean } };
export type Command = MicCommand | { command: "requestState"; args: Record<string, never> };

function groups(v: unknown): PluginGroup[] {
  return list(v, 5, value => {
    const o = object(value); requireValue(Number.isInteger(o.index) && Number(o.index) >= 1 && Number(o.index) <= 5);
    return { index: Number(o.index), off: bool(o.off) };
  }, g => g.index);
}
function sends(v: unknown): Send[] {
  return list(v, 4, value => { const o = object(value); return { fxId: uuid(o.fxId), amount: fraction(o.amount), pre: bool(o.pre) }; }, s => s.fxId);
}
function channel(value: unknown): Channel {
  const o = object(value);
  return { id: uuid(o.id), name: str(o.name), on: bool(o.on), muteGroup: bool(o.muteGroup), pluginGroups: groups(o.pluginGroups), sends: sends(o.sends) };
}
function fx(value: unknown): Fx {
  const o = object(value); return { id: uuid(o.id), name: str(o.name), muteGroup: bool(o.muteGroup), return: fraction(o.return) };
}
export function validateProjection(value: unknown): Projection {
  const o = object(value), session = object(o.session), audio = object(o.audio), mute = object(o.muteGroups);
  const p = {
    session: { name: str(session.name), dirty: bool(session.dirty) }, audio: { running: bool(audio.running) },
    muteGroups: { mic: bool(mute.mic), fx: bool(mute.fx) }, channels: list(o.channels, 8, channel, c => c.id), fx: list(o.fx, 4, fx, f => f.id)
  };
  requireValue(p.channels.every(c => c.sends.every(s => p.fx.some(f => f.id === s.fxId))));
  return p;
}
function changes(value: unknown): Changes {
  const o = object(value), result: Changes = {};
  if (o.session !== undefined) {
    const s = object(o.session); result.session = {};
    if (s.name !== undefined) result.session.name = str(s.name);
    if (s.dirty !== undefined) result.session.dirty = bool(s.dirty);
  }
  if (o.audio !== undefined) { const a = object(o.audio); result.audio = {}; if (a.running !== undefined) result.audio.running = bool(a.running); }
  if (o.muteGroups !== undefined) {
    const m = object(o.muteGroups); result.muteGroups = {};
    if (m.mic !== undefined) result.muteGroups.mic = bool(m.mic);
    if (m.fx !== undefined) result.muteGroups.fx = bool(m.fx);
  }
  if (o.channels !== undefined) result.channels = list(o.channels, 8, value => {
    const c = object(value), patch: Partial<Channel> & { id: string } = { id: uuid(c.id) };
    if (c.name !== undefined) patch.name = str(c.name);
    if (c.on !== undefined) patch.on = bool(c.on);
    if (c.muteGroup !== undefined) patch.muteGroup = bool(c.muteGroup);
    if (c.pluginGroups !== undefined) patch.pluginGroups = groups(c.pluginGroups);
    if (c.sends !== undefined) patch.sends = sends(c.sends);
    return patch;
  }, c => c.id);
  if (o.fx !== undefined) result.fx = list(o.fx, 4, value => {
    const f = object(value), patch: Partial<Fx> & { id: string } = { id: uuid(f.id) };
    if (f.name !== undefined) patch.name = str(f.name);
    if (f.muteGroup !== undefined) patch.muteGroup = bool(f.muteGroup);
    if (f.return !== undefined) patch.return = fraction(f.return);
    return patch;
  }, f => f.id);
  return result;
}
function context(o: ObjectValue): Context { return { instanceId: uuid(o.instanceId), sessionId: uuid(o.sessionId), revision: rev(o.revision) }; }
export function validateServerMessage(value: unknown): ServerMessage {
  const o = object(value);
  if (o.v !== VERSION) throw new ProtocolError("UNSUPPORTED_VERSION");
  const type = str(o.type), v = VERSION;
  requireValue(!["hello", "command", "ping"].includes(type));
  switch (type) {
    case "helloAck": {
      const server = object(o.server);
      requireValue(Array.isArray(o.capabilities) && o.capabilities.length <= 128 && o.capabilities.every(c => typeof c === "string"));
      requireValue(rev(o.eventIntervalMs) > 0 && rev(o.heartbeatIntervalMs) > 0);
      return { v, type, id: id(o.id), instanceId: uuid(o.instanceId), server: { name: str(server.name), version: str(server.version) }, capabilities: o.capabilities as string[], eventIntervalMs: o.eventIntervalMs as number, heartbeatIntervalMs: o.heartbeatIntervalMs as number };
    }
    case "state": {
      const reason = str(o.reason);
      requireValue(["initial", "requested", "structureChanged", "sessionChanged", "resync"].includes(reason));
      return { v, type, ...context(o), reason, ...(reason === "requested" ? { requestId: id(o.requestId) } : {}), state: validateProjection(o.state) };
    }
    case "stateDelta": return { v, type, ...context(o), baseRevision: rev(o.baseRevision), changes: changes(o.changes) };
    case "ack": return { v, type, ...context(o), id: id(o.id), changed: bool(o.changed), result: object(o.result) };
    case "pong": return { v, type, ...context(o), id: id(o.id) };
    case "error": {
      const error: WireError = { v, type, id: o.id === null ? null : id(o.id), code: str(o.code), message: str(o.message), retryable: bool(o.retryable) };
      if (o.instanceId !== undefined) error.instanceId = uuid(o.instanceId);
      if (o.sessionId !== undefined) error.sessionId = uuid(o.sessionId);
      if (o.revision !== undefined) error.revision = rev(o.revision);
      if (o.retryAfterMs !== undefined) rev(o.retryAfterMs);
      if (o.supportedVersions !== undefined) requireValue(Array.isArray(o.supportedVersions) && o.supportedVersions.every(n => isRevision(n) && n > 0));
      return error;
    }
    case "serverStatus": {
      requireValue(o.status === "disabled" || o.status === "stopping");
      requireValue(["controlDisabled", "shutdown", "restart"].includes(str(o.reason)));
      return { v, type, instanceId: uuid(o.instanceId), status: o.status, reason: str(o.reason) };
    }
    default: return { v, type: "unknown", wireType: type, ...context(o) };
  }
}

/** Bound bytes and depth BEFORE JSON.parse; decode only complete lines, so split UTF-8 is lossless. */
export function parseJson(bytes: Uint8Array): unknown {
  const text = new TextDecoder("utf-8", { fatal: true, ignoreBOM: true }).decode(bytes);
  if (!text.trim() || text.charCodeAt(0) === 0xfeff) throw new ProtocolError("INVALID_JSON");
  let depth = 0, quoted = false, escaped = false;
  for (const char of text) {
    if (quoted) { if (escaped) escaped = false; else if (char === "\\") escaped = true; else if (char === '"') quoted = false; }
    else if (char === '"') quoted = true;
    else if (char === "{" || char === "[") { if (++depth > 16) throw new ProtocolError("INVALID_ARGUMENT"); }
    else if (char === "}" || char === "]") depth--;
  }
  const value: unknown = JSON.parse(text);
  requireValue(isObject(value));
  return value;
}
export class NdjsonDecoder {
  private readonly pending = Buffer.alloc(MAX_LINE_BYTES);
  private used = 0;
  push(bytes: Buffer): unknown[] {
    const messages: unknown[] = [];
    let offset = 0;
    while (offset < bytes.length) {
      const end = bytes.indexOf(10, offset), stop = end < 0 ? bytes.length : end;
      if (this.used + stop - offset > MAX_LINE_BYTES) throw new ProtocolError("MESSAGE_TOO_LARGE");
      bytes.copy(this.pending, this.used, offset, stop); this.used += stop - offset;
      if (end < 0) break;
      messages.push(parseJson(this.pending.subarray(0, this.used))); this.used = 0; offset = end + 1;
    }
    return messages;
  }
  finish(): void { if (this.used) throw new ProtocolError("INVALID_JSON"); }
}
