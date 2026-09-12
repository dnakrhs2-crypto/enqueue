import { EventEmitter } from "node:events";
import { watch, type FSWatcher } from "node:fs";
import { open } from "node:fs/promises";
import { join, dirname } from "node:path";
import { createConnection, type Socket } from "node:net";
import { performance } from "node:perf_hooks";
import { isObject, isUuid, isRevision, NdjsonDecoder, parseJson, ProtocolError, validAck, validateServerMessage, type Ack, type Command, type HelloAck, type ServerMessage } from "./protocol.js";
import { StateStore } from "./state-store.js";

export type ConnectionStatus = "disconnected" | "connecting" | "checking" | "ready" | "disabled" | "version";
export type Discovery = {
  schemaVersion: 1; app: "LiveMix"; appVersion: string; instanceId: string; pid: number;
  state: "starting" | "ready" | "disabled" | "error" | "stopped";
  host: "127.0.0.1"; port?: number; token?: string; updatedAt: string; heartbeat: number; leaseMs: number;
};
export function validateDiscovery(value: unknown): Discovery {
  if (!isObject(value) || value.schemaVersion !== 1 || value.app !== "LiveMix" || typeof value.appVersion !== "string"
    || !isUuid(value.instanceId) || !isRevision(value.pid) || value.pid === 0 || value.host !== "127.0.0.1"
    || !["starting", "ready", "disabled", "error", "stopped"].includes(String(value.state))
    || typeof value.updatedAt !== "string" || !/^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d(?:\.\d{1,3})?Z$/.test(value.updatedAt)
    || !Number.isFinite(Date.parse(value.updatedAt)) || !isRevision(value.heartbeat) || value.leaseMs !== 15000)
    throw new ProtocolError("INVALID_DISCOVERY");
  if (value.state === "ready" && (!Number.isInteger(value.port) || Number(value.port) < 1 || Number(value.port) > 65535
    || typeof value.token !== "string" || !/^[A-Za-z0-9_-]{43}$/.test(value.token)
    || Buffer.from(value.token, "base64url").toString("base64url") !== value.token)) throw new ProtocolError("INVALID_DISCOVERY");
  if (value.state !== "ready" && (value.token !== undefined || value.port !== undefined)) throw new ProtocolError("INVALID_DISCOVERY");
  return value as Discovery;
}
export async function readDiscovery(path: string): Promise<Discovery> {
  const file = await open(path, "r");
  try {
    const stat = await file.stat();
    if (!stat.isFile() || stat.size > 8192) throw new ProtocolError("DISCOVERY_TOO_LARGE");
    const bytes = Buffer.alloc(8192);
    let total = 0;
    while (total < stat.size) {
      const { bytesRead } = await file.read(bytes, total, stat.size - total, total);
      if (!bytesRead) break;
      total += bytesRead;
    }
    if ((await file.stat()).size !== stat.size) throw new ProtocolError("DISCOVERY_CHANGED");
    return validateDiscovery(parseJson(bytes.subarray(0, total)));
  } finally { await file.close(); }
}

/** A stale/future roaming copy needs an advancing heartbeat, not repeated reads of its timestamp. */
export class DiscoveryLease {
  private previous: { instance: string; heartbeat: number; observed: number; trusted: boolean; wall: number } | undefined;
  fresh(d: Discovery, wall = Date.now(), mono = performance.now()): boolean {
    const p = this.previous, age = wall - Date.parse(d.updatedAt);
    const wallFresh = age >= 0 && age < d.leaseMs;
    if (!p || p.instance !== d.instanceId) {
      this.previous = { instance: d.instanceId, heartbeat: d.heartbeat, observed: mono - (wallFresh ? age : 0), trusted: wallFresh, wall };
    } else if (d.heartbeat > p.heartbeat) {
      this.previous = { instance: d.instanceId, heartbeat: d.heartbeat, observed: mono, trusted: mono - p.observed < d.leaseMs, wall };
    } else if (d.heartbeat < p.heartbeat) {
      this.previous = { instance: d.instanceId, heartbeat: d.heartbeat, observed: mono, trusted: false, wall };
    } else if (Math.abs((wall - p.wall) - (mono - p.observed)) > d.leaseMs && !wallFresh) {
      p.trusted = false;
    }
    return !!this.previous?.trusted && mono - this.previous.observed < d.leaseMs;
  }
}
export const reconnectDelay = (attempt: number, random = Math.random): number => Math.round(Math.min(8000, 250 * 2 ** Math.min(attempt, 5)) * (0.8 + random() * 0.4));
export class ConnectionError extends Error { constructor(readonly code: string) { super(code); } }
type Pending = { command: Command; sessionId?: string; resolve: (ack: Ack) => void; reject: (error: ConnectionError) => void; timer: ReturnType<typeof setTimeout> };
export type ConnectionOptions = {
  // Dependency injection is for in-process tests only. The plugin always uses the fixed APPDATA location.
  discoveryPath?: string;
  random?: () => number;
  timing?: Partial<{ poll: number; request: number; hello: number; ping: number; pong: number; stable: number }>;
};

export class LiveMixConnection extends EventEmitter {
  readonly store = new StateStore();
  status: ConnectionStatus = "disconnected";
  readonly path: string;
  private socket: Socket | undefined;
  private watcher: FSWatcher | undefined;
  private discovery: Discovery | undefined;
  private readonly lease = new DiscoveryLease();
  private hello: HelloAck | undefined;
  private sequence = 0;
  private pending = new Map<string, Pending>();
  private running = false;
  private reading = false;
  private readAgain = false;
  private attempt = 0;
  private authReread = false;
  private retryAt = 0;
  private resyncId: string | undefined;
  private pingId: string | undefined;
  private pollTimer: ReturnType<typeof setInterval> | undefined;
  private pingTimer: ReturnType<typeof setInterval> | undefined;
  private retryTimer: ReturnType<typeof setTimeout> | undefined;
  private deadline: ReturnType<typeof setTimeout> | undefined;
  private pongTimer: ReturnType<typeof setTimeout> | undefined;
  private stableTimer: ReturnType<typeof setTimeout> | undefined;
  private leaseTimer: ReturnType<typeof setTimeout> | undefined;
  private watchTimer: ReturnType<typeof setTimeout> | undefined;
  private resyncTimer: ReturnType<typeof setTimeout> | undefined;
  private readonly timing;
  constructor(private readonly options: ConnectionOptions = {}) {
    super();
    this.path = options.discoveryPath ?? (process.env.APPDATA ? join(process.env.APPDATA, "LiveMix", "control", "discovery.json") : "");
    this.timing = { poll: 1000, request: 2000, hello: 3000, ping: 5000, pong: 3000, stable: 10000, ...options.timing };
  }
  get ready(): boolean { return this.status === "ready" && this.store.synced; }
  get capabilities(): readonly string[] { return this.hello?.capabilities ?? []; }
  start(): void {
    if (this.running) return;
    this.running = true;
    this.pollTimer = setInterval(() => { if (!this.ready) void this.refresh(); }, this.timing.poll);
    void this.refresh();
  }
  stop(): void {
    this.running = false; this.watcher?.close(); this.watcher = undefined;
    clearInterval(this.pollTimer); clearTimeout(this.watchTimer); clearTimeout(this.leaseTimer); clearTimeout(this.retryTimer);
    this.close("STOPPED", "disconnected", false);
  }
  private setStatus(status: ConnectionStatus): void { if (status !== this.status) { this.status = status; this.emit("status", status); } }
  private watchDirectory(): void {
    if (this.watcher || !this.path || !this.running) return;
    try {
      this.watcher = watch(dirname(this.path), () => {
        if (!this.watchTimer) this.watchTimer = setTimeout(() => { this.watchTimer = undefined; void this.refresh(true); }, 30);
      });
      this.watcher.on("error", () => { this.watcher?.close(); this.watcher = undefined; });
    } catch { /* Parent may not exist yet; the 1 s poll will try again. */ }
  }
  private async refresh(fromWatch = false): Promise<void> {
    if (!this.running || !this.path) return;
    if (this.reading) { this.readAgain = true; return; }
    this.reading = true;
    this.watchDirectory();
    try {
      const d = await readDiscovery(this.path);
      if (!this.running) return;
      const previous = this.discovery;
      const endpointChanged = !previous || d.instanceId !== previous.instanceId || d.port !== previous.port || d.token !== previous.token || d.state !== previous.state;
      const advanced = endpointChanged || d.heartbeat !== previous?.heartbeat;
      this.discovery = d;
      this.emit("discovery", { state: d.state, heartbeat: d.heartbeat }); // No token, path or raw JSON.
      if (!this.lease.fresh(d)) { this.close("LEASE_EXPIRED", "checking", false); return; }
      clearTimeout(this.leaseTimer);
      this.leaseTimer = setTimeout(() => void this.refresh(), 1000);
      if (d.state !== "ready") {
        this.close("NOT_READY", d.state === "disabled" ? "disabled" : d.state === "starting" ? "checking" : "disconnected", false);
        return;
      }
      if (endpointChanged && this.socket) this.close("INSTANCE_CHANGED", "disconnected", false);
      if (endpointChanged || (fromWatch && advanced)) { this.retryAt = 0; clearTimeout(this.retryTimer); this.retryTimer = undefined; }
      if (!this.socket && performance.now() >= this.retryAt) this.connect(d);
    } catch {
      if (this.running) this.close("DISCOVERY_UNAVAILABLE", "disconnected", false);
    } finally {
      this.reading = false;
      if (this.readAgain) { this.readAgain = false; void this.refresh(); }
    }
  }
  private connect(d: Discovery): void {
    if (!this.running || d.state !== "ready" || !d.port || !d.token) return;
    this.setStatus("connecting"); this.sequence = 0; this.hello = undefined;
    const decoder = new NdjsonDecoder(), socket = createConnection({ host: "127.0.0.1", port: d.port });
    this.socket = socket; socket.setNoDelay(true);
    this.emit("attempt");
    this.deadline = setTimeout(() => this.close("HANDSHAKE_TIMEOUT"), this.timing.hello);
    socket.on("connect", () => {
      if (this.socket !== socket) return;
      this.send({ v: 1, type: "hello", id: this.nextId(), supportedVersions: [1], token: d.token, client: { name: "LiveMix Stream Deck", version: "1.1.0" } });
    });
    socket.on("data", bytes => {
      if (this.socket !== socket) return;
      try {
        for (const raw of decoder.push(Buffer.isBuffer(bytes) ? bytes : Buffer.from(bytes))) { if (this.socket !== socket) break; this.receive(validateServerMessage(raw), d); }
      } catch (error) { this.close(error instanceof ProtocolError ? error.code : "INVALID_MESSAGE", error instanceof ProtocolError && error.code === "UNSUPPORTED_VERSION" ? "version" : "disconnected"); }
    });
    socket.on("end", () => { if (this.socket === socket) this.close("EOF"); });
    socket.on("error", () => { if (this.socket === socket) this.close("SOCKET_ERROR"); });
    socket.on("close", () => { if (this.socket === socket) this.close("EOF"); });
  }
  private receive(m: ServerMessage, d: Discovery): void {
    if (m.type === "error" && m.code === "AUTH_FAILED") {
      const reread = !this.authReread; this.authReread = true; this.close("AUTH_FAILED");
      if (reread) void this.refresh();
      return;
    }
    if (m.type === "error" && m.code === "UNSUPPORTED_VERSION") { this.close(m.code, "version"); return; }
    if (!this.hello) {
      if (m.type !== "helloAck" || m.id !== "1" || m.instanceId !== d.instanceId || m.server.name !== "LiveMix") throw new ProtocolError("HANDSHAKE_FAILED");
      this.hello = m; this.setStatus("checking"); return;
    }
    if (m.type === "helloAck" || (m.instanceId !== undefined && m.instanceId !== d.instanceId)) throw new ProtocolError("INSTANCE_CHANGED");
    if (m.type === "serverStatus") { this.close("SERVER_STOPPING", m.status === "disabled" ? "disabled" : "disconnected", false); return; }
    if (!this.store.snapshot && m.type !== "state" && m.type !== "error") throw new ProtocolError("STATE_REQUIRED");
    if (m.type === "state" || m.type === "stateDelta") {
      if (m.type === "state" && this.store.snapshot && this.store.snapshot.sessionId !== m.sessionId) this.cancelPending("SESSION_CHANGED");
      if (this.store.accept(m)) {
        if (m.type === "state") {
          if (this.resyncId && m.requestId !== this.resyncId) return;
          this.resyncId = undefined; clearTimeout(this.resyncTimer); clearTimeout(this.deadline);
          this.setStatus("ready");
          if (!this.pingTimer) this.pingTimer = setInterval(() => this.ping(), this.timing.ping);
          if (!this.stableTimer) this.stableTimer = setTimeout(() => { this.attempt = 0; this.authReread = false; }, this.timing.stable);
        }
      } else this.requestState();
      return;
    }
    if (m.type === "unknown") {
      if (m.sessionId !== this.store.snapshot?.sessionId) { this.cancelPending("SESSION_CHANGED"); this.store.reset(); this.requestState(); }
      else if (m.revision !== this.store.snapshot?.revision) this.requestState();
      return;
    }
    if (m.type === "pong") {
      if (m.id !== this.pingId) throw new ProtocolError("UNEXPECTED_PONG");
      this.pingId = undefined; clearTimeout(this.pongTimer);
      if (m.sessionId !== this.store.snapshot?.sessionId || m.revision > (this.store.snapshot?.revision ?? -1)) this.requestState();
      return;
    }
    if (m.type !== "ack" && m.type !== "error") return;
    const request = m.id ? this.pending.get(m.id) : undefined;
    if (!request) throw new ProtocolError("UNEXPECTED_RESPONSE");
    this.pending.delete(m.id!); clearTimeout(request.timer);
    if (m.type === "error") {
      request.reject(new ConnectionError(m.code));
      if (["CONTROL_DISABLED", "SERVER_STOPPING", "INSTANCE_CHANGED"].includes(m.code)) this.close(m.code, m.code === "CONTROL_DISABLED" ? "disabled" : "disconnected");
      else if (["SESSION_CHANGED", "REVISION_CONFLICT", "CHANNEL_NOT_FOUND", "FX_NOT_FOUND", "PLUGIN_GROUP_NOT_FOUND", "INTERNAL_ERROR"].includes(m.code)) {
        if (m.code === "SESSION_CHANGED") { this.cancelPending(m.code); this.store.reset(); }
        this.requestState();
      } else if (request.command.command === "requestState") this.close(m.code);
      return;
    }
    if ((request.sessionId && m.sessionId !== request.sessionId)
      || !validAck(request.command, m)) {
      request.reject(new ConnectionError("INVALID_ACK")); throw new ProtocolError("INVALID_ACK");
    }
    request.resolve(m); // Deliberately do not patch the state store from this result.
  }
  private nextId(): string {
    if (this.sequence >= Number.MAX_SAFE_INTEGER) { this.close("ID_EXHAUSTED"); throw new ConnectionError("ID_EXHAUSTED"); }
    return String(++this.sequence);
  }
  private send(message: unknown): void {
    if (!this.socket || this.socket.destroyed) throw new ConnectionError("DISCONNECTED");
    this.socket.write(JSON.stringify(message) + "\n");
  }
  command(command: Command, ifRevision?: number): Promise<Ack> {
    if (!this.ready) return Promise.reject(new ConnectionError("DISCONNECTED"));
    return this.issue(command, ifRevision);
  }
  private issue(command: Command, ifRevision?: number): Promise<Ack> {
    if (!this.socket || !this.hello) return Promise.reject(new ConnectionError("DISCONNECTED"));
    let id: string;
    try { id = this.nextId(); } catch { return Promise.reject(new ConnectionError("ID_EXHAUSTED")); }
    const sessionId = command.command === "requestState" ? undefined : this.store.snapshot?.sessionId;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => this.close("REQUEST_TIMEOUT"), this.timing.request);
      this.pending.set(id, { command, sessionId, resolve, reject, timer });
      try { this.send({ v: 1, type: "command", id, instanceId: this.hello!.instanceId, ...(sessionId ? { sessionId } : {}), ...(ifRevision !== undefined ? { ifRevision } : {}), ...command }); }
      catch { this.close("SOCKET_ERROR"); }
    });
  }
  requestState(): void {
    if (!this.socket || !this.hello || this.resyncId) return;
    this.store.invalidate(); this.setStatus("checking");
    this.resyncId = String(this.sequence + 1);
    this.resyncTimer = setTimeout(() => this.close("STATE_TIMEOUT"), this.timing.request);
    void this.issue({ command: "requestState", args: {} }).catch(() => { /* Connection/error path owns recovery. */ });
  }
  private ping(): void {
    if (!this.socket || !this.hello || this.pingId) return;
    try { this.pingId = this.nextId(); this.send({ v: 1, type: "ping", id: this.pingId, instanceId: this.hello.instanceId }); }
    catch { this.close("SOCKET_ERROR"); return; }
    this.pongTimer = setTimeout(() => this.close("PONG_TIMEOUT"), this.timing.pong);
  }
  private cancelPending(code: string): void {
    for (const p of this.pending.values()) { clearTimeout(p.timer); p.reject(new ConnectionError(code)); }
    this.pending.clear();
  }
  private close(code: string, status: ConnectionStatus = "disconnected", retry = true): void {
    const hadSocket = !!this.socket;
    const socket = this.socket; this.socket = undefined; socket?.destroy();
    clearTimeout(this.deadline); clearInterval(this.pingTimer); clearTimeout(this.pongTimer); clearTimeout(this.stableTimer); clearTimeout(this.resyncTimer);
    this.pingTimer = undefined; this.stableTimer = undefined; this.hello = undefined; this.pingId = undefined; this.resyncId = undefined;
    this.cancelPending(code);
    if (hadSocket || this.store.snapshot) this.store.reset();
    this.setStatus(status);
    if (hadSocket) this.emit("disconnect", code);
    if (!retry) { clearTimeout(this.retryTimer); this.retryTimer = undefined; }
    if (this.running && retry && hadSocket) {
      const delay = reconnectDelay(this.attempt++, this.options.random);
      this.retryAt = performance.now() + delay;
      clearTimeout(this.retryTimer);
      this.retryTimer = setTimeout(() => { this.retryTimer = undefined; void this.refresh(); }, delay);
      this.emit("backoff", delay);
    }
  }
}
