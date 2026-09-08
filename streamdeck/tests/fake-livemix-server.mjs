import assert from "node:assert/strict";
import { EventEmitter, once } from "node:events";
import { createServer } from "node:net";
import { mkdir, mkdtemp, rename, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { randomBytes } from "node:crypto";
import { fixture } from "./helpers.mjs";

export class FakeLiveMix extends EventEmitter {
  appdata = "";
  directory = "";
  discoveryPath = "";
  clients = new Set();
  received = [];
  errors = [];
  connections = 0;
  heartbeat = 0;
  publication = Promise.resolve();
  discoveryState = "ready";
  token = randomBytes(32).toString("base64url");
  behavior = { omitState: false, omitAck: false, omitPong: false, holdCommands: false, holdHello: false, split: false, version: 1, rejectAuth: false, helloInstance: "" };
  async start() {
    this.appdata = await mkdtemp(join(tmpdir(), "livemix-sd-"));
    this.directory = join(this.appdata, "LiveMix", "control");
    this.discoveryPath = join(this.directory, "discovery.json");
    await mkdir(this.directory, { recursive: true });
    this.snapshot = await fixture("initial-state"); this.helloFixture = await fixture("hello-ack");
    this.ordering = await fixture("ordering"); this.errorsFixture = await fixture("errors"); this.clientHello = await fixture("hello");
    await this.listen(); await this.publishDiscovery();
    this.heartbeatTimer = setInterval(() => { void this.publishDiscovery().catch(error => this.errors.push(error)); }, 5000);
    return this;
  }
  async listen() {
    this.server = createServer(socket => {
      const client = { socket, authenticated: false, lastId: 0, published: structuredClone(this.snapshot), pending: Buffer.alloc(0) };
      this.clients.add(client); this.connections++; this.emit("connection");
      socket.setNoDelay(true);
      socket.on("error", () => {});
      socket.on("close", () => { this.clients.delete(client); this.emit("clientClosed"); });
      socket.on("data", bytes => {
        client.pending = Buffer.concat([client.pending, bytes]);
        let newline;
        while ((newline = client.pending.indexOf(10)) >= 0) {
          const line = client.pending.subarray(0, newline); client.pending = client.pending.subarray(newline + 1);
          try { this.receive(client, JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(line))); }
          catch (error) { this.errors.push(error); socket.destroy(); this.emit("failure"); }
        }
      });
    });
    this.server.listen(0, "127.0.0.1"); await once(this.server, "listening");
    this.port = this.server.address().port;
  }
  receive(client, m) {
    this.received.push(m); this.emit("message", m, client);
    assert.equal(m.v, 1); assert.match(m.id, /^[1-9][0-9]{0,15}$/); assert.ok(Number(m.id) > client.lastId); client.lastId = Number(m.id);
    if (!client.authenticated) {
      assert.equal(m.type, this.clientHello.type); assert.deepEqual(m.supportedVersions, this.clientHello.supportedVersions);
      assert.equal(typeof m.client.name, "string"); assert.equal(typeof m.client.version, "string");
      if (m.token !== this.token || this.behavior.rejectAuth) { this.send(client, { ...this.errorsFixture[0], id: m.id }); client.socket.end(); return; }
      if (this.behavior.holdHello) return;
      client.authenticated = true;
      this.send(client, { ...this.helloFixture, v: this.behavior.version, instanceId: this.behavior.helloInstance || this.snapshot.instanceId, id: m.id });
      if (!this.behavior.omitState) this.sendState(client, "initial");
      return;
    }
    assert.equal(m.instanceId, this.snapshot.instanceId);
    if (m.type === "ping") {
      if (!this.behavior.omitPong) this.send(client, { v: 1, type: "pong", id: m.id, ...this.context() });
      return;
    }
    assert.equal(m.type, "command");
    if (this.behavior.holdCommands) return;
    if (m.command === "requestState") {
      this.send(client, { ...this.ordering[6], ...this.context(), id: m.id, result: { snapshotRevision: this.snapshot.revision } });
      if (!this.behavior.omitState) this.sendState(client, "requested", m.id);
      return;
    }
    if (m.sessionId !== this.snapshot.sessionId) { this.error(client, m.id, "SESSION_CHANGED"); return; }
    if (m.ifRevision !== undefined && m.ifRevision !== this.snapshot.revision) { this.error(client, m.id, "REVISION_CONFLICT"); return; }
    const channel = this.snapshot.state.channels.find(c => c.id === m.args.channelId);
    if (!channel) { this.error(client, m.id, "CHANNEL_NOT_FOUND"); return; }
    assert.ok(m.command === "toggleChannel" || m.command === "setChannelOn");
    if (m.command === "setChannelOn") assert.equal(typeof m.args.on, "boolean");
    const on = m.command === "toggleChannel" ? !channel.on : m.args.on;
    const changed = on !== channel.on;
    if (changed) { channel.on = on; this.snapshot.revision++; }
    if (!this.behavior.omitAck) this.send(client, { ...this.ordering[2], ...this.context(), id: m.id, changed, result: { on } });
    if (changed) this.publishDelta();
    this.emit("command", m);
  }
  context() { return { instanceId: this.snapshot.instanceId, sessionId: this.snapshot.sessionId, revision: this.snapshot.revision }; }
  send(client, message) {
    const bytes = Buffer.from(JSON.stringify(message) + "\n");
    if (this.behavior.split) {
      // Split inside the first multibyte code point and yield to the socket between pieces.
      client.outgoing = (client.outgoing ?? Promise.resolve()).then(async () => {
        if (client.socket.destroyed) return;
        const index = bytes.findIndex(byte => byte >= 0x80), cut = index < 0 ? 13 : index + 1;
        client.socket.write(bytes.subarray(0, cut));
        await new Promise(resolve => setImmediate(resolve));
        if (!client.socket.destroyed) client.socket.write(bytes.subarray(cut));
      });
    } else client.socket.write(bytes);
  }
  sendState(client, reason = "structureChanged", requestId) {
    this.send(client, { ...this.snapshot, reason, ...(requestId ? { requestId } : {}) });
    client.published = structuredClone(this.snapshot);
  }
  broadcastState(reason = "structureChanged") { for (const c of this.clients) if (c.authenticated) this.sendState(c, reason); }
  publishDelta() {
    for (const c of this.clients) if (c.authenticated && c.published.revision !== this.snapshot.revision) {
      const changes = {};
      const channels = this.snapshot.state.channels.filter(ch => JSON.stringify(ch) !== JSON.stringify(c.published.state.channels.find(old => old.id === ch.id)));
      if (channels.length) changes.channels = channels;
      for (const key of ["audio", "session", "muteGroups"]) if (JSON.stringify(c.published.state[key]) !== JSON.stringify(this.snapshot.state[key])) changes[key] = this.snapshot.state[key];
      this.send(c, { ...this.ordering[4], ...this.context(), baseRevision: c.published.revision, changes });
      c.published = structuredClone(this.snapshot);
    }
  }
  mutate(edit, full = false) {
    edit(this.snapshot); this.snapshot.revision++;
    if (full) this.broadcastState(); else this.publishDelta();
    this.emit("mutation");
  }
  error(client, id, code) { this.send(client, { v: 1, type: "error", id, code, message: "Test command rejected", retryable: false, ...this.context() }); }
  publishDiscovery(overrides = {}) {
    const value = { schemaVersion: 1, app: "LiveMix", appVersion: "0.5.3", instanceId: this.snapshot.instanceId, pid: process.pid,
      state: this.discoveryState, host: "127.0.0.1", updatedAt: new Date().toISOString(), heartbeat: ++this.heartbeat, leaseMs: 15000,
      ...(this.discoveryState === "ready" ? { port: this.port, token: this.token } : {}), ...overrides };
    const temporary = join(this.directory, `discovery-${this.heartbeat}.tmp`);
    const result = this.publication.then(async () => {
      await writeFile(temporary, JSON.stringify(value)); await rename(temporary, this.discoveryPath); this.emit("discovery");
      return value;
    });
    this.publication = result.then(() => {}, () => {});
    return result;
  }
  async setStatus(state) {
    this.discoveryState = state; await this.publishDiscovery();
    for (const c of this.clients) {
      this.send(c, { ...this.errorsFixture[state === "disabled" ? 2 : 3], instanceId: this.snapshot.instanceId }); c.socket.end();
    }
  }
  eof() { for (const c of this.clients) c.socket.end(); }
  async changePort() {
    for (const c of this.clients) c.socket.destroy();
    await new Promise(resolve => this.server.close(resolve));
    await this.listen(); await this.publishDiscovery();
  }
  async close() {
    clearInterval(this.heartbeatTimer);
    for (const c of this.clients) c.socket.destroy();
    if (this.server?.listening) await new Promise(resolve => this.server.close(resolve));
    await this.publication;
    if (this.appdata) await rm(this.appdata, { recursive: true, force: true });
    assert.deepEqual(this.errors, [], "Fake LiveMix received invalid protocol or failed I/O");
  }
}
