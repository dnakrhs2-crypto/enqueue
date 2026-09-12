import test, { type TestContext } from "node:test";
import assert from "node:assert/strict";
import { writeFile, unlink } from "node:fs/promises";
import { LiveMixConnection, DiscoveryLease, readDiscovery, reconnectDelay, validateDiscovery } from "../src/livemix/connection.js";
import { NdjsonDecoder, parseJson, validAck, validateServerMessage, type Snapshot, type Delta, type Ack, type Command } from "../src/livemix/protocol.js";
import { StateStore } from "../src/livemix/state-store.js";
import { CommandQueue } from "../src/livemix/command-queue.js";
import { FakeLiveMix } from "./fake-livemix-server.mjs";
import { fixture, nextEvent, until } from "./helpers.mjs";

async function setup(t: TestContext, behavior: Partial<FakeLiveMix["behavior"]> = {}, timing: { request?: number; hello?: number; ping?: number; pong?: number; stable?: number } = {}) {
  const server = await new FakeLiveMix().start(); Object.assign(server.behavior, behavior);
  const connection = new LiveMixConnection({ discoveryPath: server.discoveryPath, random: () => 0.5, timing: { poll: 50, ...timing } });
  t.after(async () => { connection.stop(); await server.close(); }); connection.start();
  return { server, connection, ready: () => until(connection, "status", () => connection.ready) };
}
const mic = (id: string) => ({ command: "toggleChannel" as const, args: { channelId: id } });

test("hello token, v1, instance and complete snapshot gate readiness", async t => {
  const { server, connection, ready } = await setup(t, { omitState: true });
  await until(connection, "status", () => connection.status === "checking");
  assert.equal(connection.ready, false);
  await assert.rejects(connection.command(mic(server.snapshot.state.channels[0].id)), /DISCONNECTED/);
  assert.equal(server.received[0].token, server.token); assert.deepEqual(server.received[0].supportedVersions, [1]);
  assert.equal(server.received[0].client.version, "1.1.0"); assert.ok(connection.capabilities.includes("pluginGroupsEverywhere"));
  server.broadcastState("initial"); await ready();
  assert.equal(connection.store.snapshot?.instanceId, server.snapshot.instanceId);
});

test("ACK validation follows each command result and rejects malformed group/send responses", async () => {
  const snapshot = await fixture("initial-state");
  const ack: Ack = { v: 1, type: "ack", id: "2", instanceId: snapshot.instanceId, sessionId: snapshot.sessionId, revision: 2, changed: true, result: {} };
  const cases: { command: Command; result: Record<string, unknown>; bad: Record<string, unknown> }[] = [
    { command: { command: "setAllChannelsOn", args: { on: true } }, result: { on: true, count: 3 }, bad: { on: true, count: -1 } },
    { command: { command: "toggleMuteGroup", args: { group: "mic" } }, result: { group: "mic", muted: true }, bad: { group: "fx", muted: true } },
    { command: { command: "setPluginGroupOff", args: { channelId: snapshot.state.channels[0].id, index: 2, off: false } }, result: { index: 2, off: false }, bad: { index: 1, off: false } },
    ...[{ index: 1, off: false, count: 3 }, { index: 2, off: "false", count: 3 }, { index: 2, off: false }, { index: 2, off: false, count: 1.5 }, { index: 2, off: false, count: "3" }].map(bad => ({
      command: { command: "setPluginGroupOffEverywhere" as const, args: { index: 2, off: false } }, result: { index: 2, off: false, count: 3 }, bad })),
    { command: { command: "setSend", args: { channelId: snapshot.state.channels[0].id, fxId: snapshot.state.fx[0].id, amount: 0.35 } }, result: { amount: 0.35, pre: false }, bad: { amount: 1.5, pre: false } }
  ];
  for (const c of cases) { assert.equal(validAck(c.command, { ...ack, result: c.result }), true); assert.equal(validAck(c.command, { ...ack, result: c.bad }), false); assert.equal(validAck(c.command, ack), false); }
});

test("plugin group everywhere wire: count, no-op ACK, canonical delta, validation and missing group error", async t => {
  const { server, connection, ready } = await setup(t); await ready();
  server.mutate((s: Snapshot) => {
    s.state.channels[0]!.pluginGroups = [{ index: 1, off: false }];
    s.state.channels.push({ ...structuredClone(s.state.channels[0]!), id: "33333333333343338333333333333333" },
      { ...structuredClone(s.state.channels[0]!), id: "55555555555545558555555555555555", pluginGroups: [] });
  }, true);
  await until(connection.store, "change", () => connection.store.snapshot?.revision === server.snapshot.revision);
  const revision = server.snapshot.revision, command: Command = { command: "setPluginGroupOffEverywhere", args: { index: 1, off: true } };
  server.behavior.omitDelta = true;
  const ack = await connection.command(command, revision);
  assert.deepEqual(ack.result, { index: 1, off: true, count: 2 }); assert.equal(ack.changed, true); assert.equal(ack.revision, revision + 1);
  assert.equal(connection.store.snapshot?.revision, revision);
  assert.equal(connection.store.snapshot?.state.channels[0]!.pluginGroups[0]!.off, false, "ACK never patches the cache");
  server.behavior.omitDelta = false; server.publishDelta();
  await until(connection.store, "change", () => connection.store.snapshot?.revision === ack.revision);
  assert.ok(connection.store.snapshot?.state.channels.every(c => c.pluginGroups.every(g => g.off)));
  const same = await connection.command(command, ack.revision);
  assert.equal(same.changed, false); assert.equal(same.revision, ack.revision); assert.deepEqual(same.result, ack.result);
  for (const args of [{ index: 0, off: true }, { index: 6, off: true }, { index: 1.5, off: true }, { index: "1", off: true }, { index: 1, off: "true" }, { index: 1 }]) {
    await assert.rejects(connection.command({ command: "setPluginGroupOffEverywhere", args } as Command), /INVALID_ARGUMENT/);
  }
  await assert.rejects(connection.command({ command: "setPluginGroupOffEverywhere", args: { index: 3, off: false } }), /PLUGIN_GROUP_NOT_FOUND/);
  assert.equal(server.snapshot.revision, ack.revision);
});
test("UTF-8 byte splitting, coalesced messages, CRLF and framing limits", async () => {
  const source = await fixture("initial-state"), bytes = Buffer.from(JSON.stringify(source) + "\r\n");
  const decoder = new NdjsonDecoder(), messages: unknown[] = [];
  for (const byte of bytes) messages.push(...decoder.push(Buffer.from([byte])));
  assert.deepEqual(validateServerMessage(messages[0]), source);
  assert.equal(new NdjsonDecoder().push(Buffer.concat([bytes, bytes])).length, 2);
  for (const bad of [Buffer.from("\n"), Buffer.from("[]\n"), Buffer.from("\ufeff{}\n"), Buffer.from([0xff, 10]), Buffer.from(" ".repeat(65537))]) assert.throws(() => new NdjsonDecoder().push(bad));
  const partial = new NdjsonDecoder(); partial.push(Buffer.from("{}")); assert.throws(() => partial.finish());
  assert.throws(() => parseJson(Buffer.from('{"x":'.repeat(17) + "0" + "}".repeat(17))));
});
test("real TCP chunks preserve Korean channel names", async t => {
  const { server, connection, ready } = await setup(t, { split: true }); await ready();
  assert.equal(connection.store.snapshot?.state.channels[0]?.name, server.snapshot.state.channels[0].name);
});
test("unsupported version and wrong hello instance never become ready", async t => {
  for (const behavior of [{ version: 2 }, { helloInstance: "cccccccccccc4ccc8ccccccccccccccc" }]) await t.test(JSON.stringify(behavior), async t => {
    const { connection } = await setup(t, behavior);
    await nextEvent(connection, "disconnect"); assert.equal(connection.ready, false); assert.equal(connection.store.snapshot, undefined);
  });
});
test("hello and full-state deadlines disconnect incomplete handshakes", async t => {
  for (const behavior of [{ holdHello: true }, { omitState: true }]) await t.test(JSON.stringify(behavior), async t => {
    const { connection } = await setup(t, behavior, { hello: 100 });
    assert.deepEqual(await nextEvent(connection, "disconnect"), ["HANDSHAKE_TIMEOUT"]);
  });
});
test("state before helloAck and malformed required fields close the connection", async t => {
  const { server, connection } = await setup(t, { holdHello: true });
  await until(server, "message", () => server.received.length > 0);
  const closed = nextEvent(connection, "disconnect"); server.broadcastState("initial");
  // The held client has not authenticated; send the unsolicited frame explicitly.
  server.send([...server.clients][0], server.snapshot); await closed; assert.equal(connection.ready, false);
  const ack = await fixture("hello-ack"); delete ack.capabilities; assert.throws(() => validateServerMessage(ack));
  assert.throws(() => validateServerMessage({ v: 1, type: "ack", id: "01" }));
});
test("delta gaps and unknown IDs request one full state without patching old state", async t => {
  const { server, connection, ready } = await setup(t); await ready();
  const old = connection.store.snapshot!, requests = () => server.received.filter((m: { command?: string }) => m.command === "requestState");
  const c = [...server.clients][0];
  server.send(c, { v: 1, type: "stateDelta", ...server.context(), baseRevision: old.revision + 4, revision: old.revision + 5, changes: { channels: [{ id: old.state.channels[0]!.id, on: false }] } });
  await until(server, "message", () => requests().length === 1); await ready();
  assert.equal(connection.store.snapshot?.state.channels[0]?.on, true);
  server.send(c, { v: 1, type: "stateDelta", ...server.context(), baseRevision: old.revision, revision: old.revision + 1, changes: { channels: [{ id: "cccccccccccc4ccc8ccccccccccccccc", on: false }] } });
  await until(server, "message", () => requests().length === 2); await ready();
});
test("unknown future events that advance revision cause resync", async t => {
  const { server, connection, ready } = await setup(t); await ready();
  server.send([...server.clients][0], { v: 1, type: "futureEvent", ...server.context(), revision: server.snapshot.revision + 1 });
  await until(server, "message", () => server.received.some((m: { command?: string }) => m.command === "requestState")); await ready();
  assert.ok(connection.ready);
});
test("EOF immediately discards cached readiness although discovery still says ready", async t => {
  const { server, connection, ready } = await setup(t); await ready();
  const closed = nextEvent(connection, "disconnect"); server.eof(); assert.deepEqual(await closed, ["EOF"]);
  assert.equal(connection.ready, false); assert.equal(connection.store.snapshot, undefined);
  assert.equal((await readDiscovery(server.discoveryPath)).state, "ready");
});
test("discovery enforces schema, loopback, token, 8 KiB limit and missing-file recovery", async t => {
  const { server, connection, ready } = await setup(t); await ready();
  const valid = await readDiscovery(server.discoveryPath);
  for (const patch of [{ schemaVersion: 2 }, { host: "localhost" }, { host: "192.168.1.2" }, { port: 0 }, { token: "short" }, { leaseMs: 0 }, { updatedAt: "bad" }, { instanceId: "0".repeat(32) }]) assert.throws(() => validateDiscovery({ ...valid, ...patch }));
  await writeFile(server.discoveryPath, "x".repeat(8193)); await assert.rejects(readDiscovery(server.discoveryPath), /DISCOVERY_TOO_LARGE/);
  await until(connection, "status", () => !connection.ready);
  await unlink(server.discoveryPath); await server.publishDiscovery(); await ready();
});
test("lease expires, repeated reads do not renew it; clock changes need new heartbeat", async t => {
  const { server, ready } = await setup(t); await ready(); const discovery = await readDiscovery(server.discoveryPath);
  const wall = Date.parse(discovery.updatedAt), lease = new DiscoveryLease();
  assert.equal(lease.fresh(discovery, wall, 0), true);
  assert.equal(lease.fresh(discovery, wall + 14999, 14999), true);
  assert.equal(lease.fresh(discovery, wall + 15001, 15001), false);
  for (const offset of [-60000, 60000]) {
    const copy = new DiscoveryLease(); assert.equal(copy.fresh(discovery, wall + offset, 0), false);
    assert.equal(copy.fresh(discovery, wall + offset + 1000, 1000), false);
    assert.equal(copy.fresh({ ...discovery, heartbeat: discovery.heartbeat + 1 }, wall + offset + 5000, 5000), true);
  }
});
test("fresh disabled, stopped and stale disabled discovery have distinct readiness", async t => {
  const { server, connection, ready } = await setup(t); await ready();
  await server.setStatus("disabled"); await until(connection, "status", () => connection.status === "disabled");
  await assert.rejects(connection.command(mic(server.snapshot.state.channels[0].id)));
  await server.setStatus("stopped"); await until(connection, "status", () => connection.status === "disconnected");
  connection.stop();
  server.discoveryState = "disabled"; await server.publishDiscovery({ updatedAt: new Date(Date.now() - 60000).toISOString() });
  const stale = new LiveMixConnection({ discoveryPath: server.discoveryPath }); t.after(() => stale.stop()); stale.start();
  await until(stale, "status", () => stale.status === "checking"); assert.equal(stale.ready, false);
});
test("port changes replace the single TCP connection and take a new full state", async t => {
  const { server, connection, ready } = await setup(t); await ready(); const before = server.connections;
  await server.changePort(); await until(server, "connection", () => server.connections > before); await ready();
  assert.equal(connection.store.snapshot?.revision, server.snapshot.revision);
});
test("AUTH_FAILED re-reads discovery and recovers with a rotated token", async t => {
  const { server, connection, ready } = await setup(t, { rejectAuth: true });
  assert.deepEqual(await nextEvent(connection, "disconnect"), ["AUTH_FAILED"]);
  server.behavior.rejectAuth = false; server.token = Buffer.alloc(32, 7).toString("base64url"); await server.publishDiscovery();
  await ready(); assert.equal(server.received.at(-1).token, server.token); assert.equal(connection.ready, true);
});
test("backoff doubles to 8 seconds and jitter stays within ±20 percent", () => {
  assert.deepEqual(Array.from({ length: 8 }, (_, i) => reconnectDelay(i, () => 0.5)), [250, 500, 1000, 2000, 4000, 8000, 8000, 8000]);
  assert.equal(reconnectDelay(0, () => 0), 200); assert.equal(reconnectDelay(10, () => 1), 9600);
});
test("repeated auth failure observes backoff; duplicate watch events do not create a storm", async t => {
  const { connection, server } = await setup(t, { rejectAuth: true }); const delays: number[] = [];
  connection.on("backoff", (delay: number) => delays.push(delay));
  await until(connection, "backoff", () => delays.length >= 3);
  assert.deepEqual(delays.slice(0, 3), [250, 500, 1000]); assert.equal(server.received.length, 3);
});
test("2-second default request timeout drops queued input; reconnect never replays it", async t => {
  const { server, connection, ready } = await setup(t, { holdCommands: true }); await ready();
  const queue = new CommandQueue(connection); t.after(() => queue.dispose()); const target = server.snapshot.state.channels[0].id;
  const first = queue.enqueue(target, "key-a", () => mic(target)), second = queue.enqueue(target, "key-b", () => mic(target));
  const failures = Promise.all([assert.rejects(first), assert.rejects(second)]);
  assert.deepEqual(await nextEvent(connection, "disconnect", 3000), ["REQUEST_TIMEOUT"]); await failures;
  server.behavior.holdCommands = false; await ready();
  assert.equal(server.received.filter((m: { type: string }) => m.type === "command").length, 1);
  assert.equal(server.snapshot.state.channels[0].on, true);
});
test("ping IDs share request sequence, pong timeout disconnects", async t => {
  const { server, connection, ready } = await setup(t, {}, { ping: 60, pong: 60 }); await ready();
  await until(server, "message", () => server.received.some((m: { type: string }) => m.type === "ping"));
  await connection.command(mic(server.snapshot.state.channels[0].id));
  server.behavior.omitPong = true;
  assert.deepEqual(await nextEvent(connection, "disconnect"), ["PONG_TIMEOUT"]);
  const ids = server.received.map((m: { id: string }) => Number(m.id)); assert.deepEqual(ids, [...ids].sort((a, b) => a - b)); assert.equal(new Set(ids).size, ids.length);
});
test("snapshot/delta reducer replaces arrays, merges scalars, accepts empty revision jumps", async () => {
  const store = new StateStore(); const snapshot = validateServerMessage(await fixture("initial-state")) as Snapshot; store.accept(snapshot);
  const delta: Delta = { v: 1, type: "stateDelta", instanceId: snapshot.instanceId, sessionId: snapshot.sessionId, baseRevision: 1, revision: 5,
    changes: { session: { dirty: false }, muteGroups: { mic: true }, channels: [{ id: snapshot.state.channels[0]!.id, pluginGroups: [{ index: 1, off: true }], sends: [] }] } };
  assert.equal(store.accept(delta), true); assert.equal(store.snapshot?.state.session.name, "Control test");
  assert.equal(store.snapshot?.state.muteGroups.fx, false); assert.deepEqual(store.snapshot?.state.channels[0]?.pluginGroups, [{ index: 1, off: true }]);
  assert.equal(store.accept({ ...delta, baseRevision: 5, revision: 7, changes: {} }), true); assert.equal(store.snapshot?.revision, 7);
  assert.equal(store.accept({ ...delta, baseRevision: 7, revision: 8, changes: { channels: [{ id: snapshot.state.channels[0]!.id, pluginGroups: [] }] } }), true);
  assert.deepEqual(store.snapshot?.state.channels[0]?.pluginGroups, []);
});
test("computed commands serialize across contexts with ifRevision and canonical state", async t => {
  const { server, connection, ready } = await setup(t); await ready(); const queue = new CommandQueue(connection); t.after(() => queue.dispose());
  const target = server.snapshot.state.channels[0].id;
  const compute = (s: Snapshot) => ({ command: "setChannelOn" as const, args: { channelId: target, on: !s.state.channels[0]!.on } });
  await Promise.all([queue.enqueue(target, "a", compute, true), queue.enqueue(target, "b", compute, true)]);
  const commands = server.received.filter((m: { type: string }) => m.type === "command");
  assert.deepEqual(commands.map((m: { ifRevision: number }) => m.ifRevision), [1, 2]); assert.equal(server.snapshot.state.channels[0].on, true);
});
test("session boundary drops active and queued commands and replaces the cache", async t => {
  const { server, connection, ready } = await setup(t, { holdCommands: true }); await ready(); const queue = new CommandQueue(connection); t.after(() => queue.dispose());
  const target = server.snapshot.state.channels[0].id;
  const failures = Promise.all([assert.rejects(queue.enqueue(target, "a", () => mic(target))), assert.rejects(queue.enqueue(target, "b", () => mic(target)))]);
  server.snapshot.sessionId = "cccccccccccc4ccc8ccccccccccccccc"; server.snapshot.revision++; server.broadcastState("sessionChanged"); await failures;
  assert.equal(connection.store.snapshot?.sessionId, server.snapshot.sessionId);
  assert.equal(server.received.filter((m: { type: string }) => m.type === "command").length <= 1, true);
});
test("an ack never patches canonical state; an ack without the requested snapshot times out", async t => {
  const { server, connection, ready } = await setup(t, {}, { request: 100 }); await ready();
  const before = connection.store.snapshot;
  server.behavior.holdCommands = true;
  const result = connection.command(mic(server.snapshot.state.channels[0].id));
  await until(server, "message", () => server.received.length === 2);
  server.send([...server.clients][0], { ...server.ordering[2], ...server.context(), id: server.received[1].id, revision: 8, changed: true, result: { on: false } });
  await result; assert.equal(connection.store.snapshot, before); assert.equal(before?.revision, 1); assert.equal(before?.state.channels[0]?.on, true);
  server.behavior.holdCommands = false; server.behavior.omitState = true;
  const closed = nextEvent(connection, "disconnect"); connection.requestState();
  assert.deepEqual(await closed, ["STATE_TIMEOUT"]);
});
test("pending input is bounded to two unsent intents and expires after 500 ms", async t => {
  const { server, connection, ready } = await setup(t, { holdCommands: true }); await ready();
  const queue = new CommandQueue(connection); t.after(() => queue.dispose()); const target = server.snapshot.state.channels[0].id;
  const active = assert.rejects(queue.enqueue(target, "active", () => mic(target)));
  const one = assert.rejects(queue.enqueue(target, "one", () => mic(target)), /INPUT_EXPIRED/);
  const two = assert.rejects(queue.enqueue(target, "two", () => mic(target)), /INPUT_EXPIRED/);
  await assert.rejects(queue.enqueue(target, "overflow", () => mic(target)), /INPUT_BUSY/);
  await Promise.all([one, two]); connection.stop(); await active;
  assert.equal(server.received.filter((m: { type: string }) => m.type === "command").length, 1);
});
