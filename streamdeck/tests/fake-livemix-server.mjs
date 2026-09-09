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
  behavior = { omitState: false, omitDelta: false, omitAck: false, omitPong: false, holdCommands: false, holdHello: false, split: false, version: 1, rejectAuth: false, helloInstance: "" };
  constructor({ pluginGroupsEverywhere = true } = {}) { super(); this.pluginGroupsEverywhere = pluginGroupsEverywhere; }
  async start() {
    this.appdata = await mkdtemp(join(tmpdir(), "livemix-sd-"));
    this.directory = join(this.appdata, "LiveMix", "control");
    this.discoveryPath = join(this.directory, "discovery.json");
    await mkdir(this.directory, { recursive: true });
    this.snapshot = await fixture("initial-state"); this.helloFixture = await fixture("hello-ack");
    this.helloFixture.capabilities = this.helloFixture.capabilities.filter(c => c !== "pluginGroupsEverywhere");
    if (this.pluginGroupsEverywhere) this.helloFixture.capabilities.push("pluginGroupsEverywhere");
    this.helloFixture.server.version = this.pluginGroupsEverywhere ? "0.10.0" : "0.9.0";
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
    const state = this.snapshot.state, channel = state.channels.find(c => c.id === m.args.channelId), a = m.args;
    const before = JSON.stringify(state);
    let result;
    if (m.command === "setAllChannelsOn") {
      assert.equal(typeof a.on, "boolean");
      for (const c of state.channels) c.on = a.on;
      result = { on: a.on, count: state.channels.length };
    } else if (m.command === "setPluginGroupOffEverywhere") {
      if (!this.pluginGroupsEverywhere) { this.error(client, m.id, "UNKNOWN_COMMAND"); return; }
      if (!Number.isInteger(a.index) || a.index < 1 || a.index > 5 || typeof a.off !== "boolean") { this.error(client, m.id, "INVALID_ARGUMENT"); return; }
      const targets = state.channels.flatMap(c => c.pluginGroups.filter(g => g.index === a.index));
      if (!targets.length) { this.error(client, m.id, "PLUGIN_GROUP_NOT_FOUND"); return; }
      for (const group of targets) group.off = a.off;
      result = { index: a.index, off: a.off, count: targets.length };
    } else if (m.command === "toggleMuteGroup" || m.command === "setMuteGroup") {
      assert.ok(a.group === "mic" || a.group === "fx");
      if (m.command === "setMuteGroup") assert.equal(typeof a.muted, "boolean");
      state.muteGroups[a.group] = m.command === "toggleMuteGroup" ? !state.muteGroups[a.group] : a.muted;
      result = { group: a.group, muted: state.muteGroups[a.group] };
    } else {
      if (!channel) { this.error(client, m.id, "CHANNEL_NOT_FOUND"); return; }
      if (m.command === "toggleChannel" || m.command === "setChannelOn") {
        if (m.command === "setChannelOn") assert.equal(typeof a.on, "boolean");
        channel.on = m.command === "toggleChannel" ? !channel.on : a.on;
        result = { on: channel.on };
      } else if (m.command === "setPluginGroupOff") {
        assert.ok(Number.isInteger(a.index) && a.index >= 1 && a.index <= 5); assert.equal(typeof a.off, "boolean");
        const group = channel.pluginGroups.find(g => g.index === a.index);
        if (!group) { this.error(client, m.id, "PLUGIN_GROUP_NOT_FOUND"); return; }
        group.off = a.off; result = { index: a.index, off: a.off };
      } else if (m.command === "setSend") {
        if (!state.fx.some(f => f.id === a.fxId)) { this.error(client, m.id, "FX_NOT_FOUND"); return; }
        assert.ok(a.amount !== undefined || a.pre !== undefined);
        if (a.amount !== undefined) assert.ok(Number.isFinite(a.amount) && a.amount >= 0 && a.amount <= 1);
        if (a.pre !== undefined) assert.equal(typeof a.pre, "boolean");
        const send = channel.sends.find(s => s.fxId === a.fxId) ?? { fxId: a.fxId, amount: 0, pre: false };
        const amount = a.amount ?? send.amount, pre = a.pre ?? send.pre;
        if (amount !== send.amount || pre !== send.pre) {
          if (!channel.sends.includes(send)) channel.sends.push(send);
          send.amount = amount; send.pre = pre;
        }
        result = { amount, pre };
      } else assert.fail("Unknown command");
    }
    const changed = JSON.stringify(state) !== before;
    if (changed) this.snapshot.revision++;
    if (!this.behavior.omitAck) this.send(client, { ...this.ordering[2], ...this.context(), id: m.id, changed, result });
    if (changed && !this.behavior.omitDelta) this.publishDelta();
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
      const fx = this.snapshot.state.fx.filter(fx => JSON.stringify(fx) !== JSON.stringify(c.published.state.fx.find(old => old.id === fx.id)));
      if (fx.length) changes.fx = fx;
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
    const value = { schemaVersion: 1, app: "LiveMix", appVersion: this.helloFixture.server.version, instanceId: this.snapshot.instanceId, pid: process.pid,
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
