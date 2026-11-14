import test from "node:test";
import assert from "node:assert/strict";
import { fixture, until } from "./helpers.mjs";
import { validateServerMessage, validAck, type Snapshot, type Delta, type Ack, type Command } from "../src/livemix/protocol.js";
import { StateStore } from "../src/livemix/state-store.js";
import { LiveMixConnection } from "../src/livemix/connection.js";
import { displayName, keyImage, statusTitle } from "../src/ui/key-renderer.js";
import { strings } from "../src/ui/strings.js";
import { FakeLiveMix } from "./fake-livemix-server.mjs";
import { setupActions, key, svg, title, commands, alerts, options } from "./action-helpers.js";

const auxId = "66666666666646668666666666666666", otherId = "77777777777747778777777777777777";
const nextId = "cccccccccccc4ccc8ccccccccccccccc";

test("aux protocol: older snapshots default to an empty list; present auxes and ACKs are validated", async () => {
  const source = await fixture("initial-state");
  assert.deepEqual((validateServerMessage(source) as Snapshot).state.auxes, []);
  const aux = { id: auxId, name: "Monitor", on: true };
  source.state.auxes = [aux, { id: otherId, name: "Cue", on: false }];
  assert.deepEqual((validateServerMessage(source) as Snapshot).state.auxes, source.state.auxes);
  for (const auxes of [null, {}, [aux, aux], [{ ...aux, id: "bad" }], [{ ...aux, name: null }], [{ id: auxId, name: "Monitor" }], [{ ...aux, on: "true" }]]) {
    assert.throws(() => validateServerMessage({ ...source, state: { ...source.state, auxes } }), { code: "INVALID_MESSAGE" });
  }
  const ack: Ack = { v: 1, type: "ack", id: "2", instanceId: source.instanceId, sessionId: source.sessionId, revision: 2, changed: true, result: { on: false } };
  for (const command of [{ command: "toggleAux", args: { auxId } }, { command: "setAuxOn", args: { auxId, on: true } }] as const) {
    assert.equal(validAck(command, ack), true);
    assert.equal(validAck(command, { ...ack, changed: false, result: { on: true } }), true);
    for (const result of [{}, { on: "false" }, { on: 1 }, { on: null }]) assert.equal(validAck(command, { ...ack, result }), false);
  }
});

test("aux state: partial name/ON deltas merge by ID; unknown IDs invalidate until a full structure update", async () => {
  const source = await fixture("initial-state");
  source.state.auxes = [{ id: auxId, name: "Monitor", on: true }, { id: otherId, name: "Cue", on: false }];
  const snapshot = validateServerMessage(source) as Snapshot, store = new StateStore(); store.accept(snapshot);
  const delta = { v: 1, type: "stateDelta", instanceId: snapshot.instanceId, sessionId: snapshot.sessionId,
    baseRevision: snapshot.revision, revision: snapshot.revision + 1, changes: { auxes: [{ id: auxId, on: false }] } };
  assert.equal(store.accept(validateServerMessage(delta) as Delta), true);
  assert.deepEqual(store.snapshot!.state.auxes, [{ id: auxId, name: "Monitor", on: false }, snapshot.state.auxes[1]]);
  const rename = { ...delta, baseRevision: delta.revision, revision: delta.revision + 1, changes: { auxes: [{ id: auxId, name: "Stage" }] } };
  const parsed = validateServerMessage(rename) as Delta;
  assert.equal("on" in parsed.changes.auxes![0]!, false); assert.equal(store.accept(parsed), true);
  assert.deepEqual(store.snapshot!.state.auxes[0], { id: auxId, name: "Stage", on: false });
  for (const auxes of [null, [{ id: auxId, on: "false" }], [{ id: auxId, name: null }], [{ id: auxId }, { id: auxId }]]) {
    assert.throws(() => validateServerMessage({ ...delta, changes: { auxes } }), { code: "INVALID_MESSAGE" });
  }
  const before = structuredClone(store.snapshot);
  assert.equal(store.accept(validateServerMessage({ ...delta, baseRevision: rename.revision, revision: rename.revision + 1,
    changes: { auxes: [{ id: nextId, on: true }] } }) as Delta), false);
  assert.equal(store.synced, false); assert.deepEqual(store.snapshot, before);
  const auxes = [snapshot.state.auxes[1], { id: nextId, name: "Replacement", on: true }];
  assert.equal(store.accept(validateServerMessage({ ...snapshot, reason: "structureChanged", revision: rename.revision + 2,
    state: { ...snapshot.state, auxes } }) as Snapshot), true);
  assert.equal(store.synced, true); assert.deepEqual(store.snapshot!.state.auxes, auxes);
  assert.deepEqual(store.snapshot!.state.channels, snapshot.state.channels); assert.deepEqual(store.snapshot!.state.fx, snapshot.state.fx);
});

test("aux wire: toggle/set results, no-op ACK, canonical delta and missing-target errors", async t => {
  const server = await new FakeLiveMix({ auxOn: true }).start();
  const connection = new LiveMixConnection({ discoveryPath: server.discoveryPath });
  t.after(async () => { connection.stop(); await server.close(); }); connection.start();
  await until(connection, "status", () => connection.ready);
  assert.equal(server.helloFixture.server.version, "0.24.1"); assert.equal(connection.capabilities.at(-1), "auxOn");
  const revision = server.snapshot.revision;
  server.behavior.omitDelta = true;
  const ack = await connection.command({ command: "toggleAux", args: { auxId } });
  assert.deepEqual(ack.result, { on: false }); assert.equal(ack.changed, true); assert.equal(ack.revision, revision + 1);
  assert.equal(connection.store.snapshot!.revision, revision); assert.equal(connection.store.snapshot!.state.auxes[0]!.on, true, "ACK never patches canonical state");
  server.behavior.omitDelta = false; server.publishDelta();
  await until(connection.store, "change", () => connection.store.snapshot!.revision === ack.revision);
  assert.equal(connection.store.snapshot!.state.auxes[0]!.on, false);
  assert.deepEqual(server.sent.find(m => m.type === "stateDelta").changes, { auxes: [{ id: auxId, on: false }] });
  const same = await connection.command({ command: "setAuxOn", args: { auxId, on: false } });
  assert.equal(same.changed, false); assert.equal(same.revision, ack.revision); assert.deepEqual(same.result, { on: false });
  const restored = await connection.command({ command: "setAuxOn", args: { auxId, on: true } });
  assert.equal(restored.changed, true); assert.deepEqual(restored.result, { on: true });
  await until(connection.store, "change", () => connection.store.snapshot!.revision === restored.revision);
  assert.equal(connection.store.snapshot!.state.auxes[0]!.on, true);
  for (const args of [{ on: true }, { auxId: "bad", on: true }, { auxId }, { auxId, on: "true" }]) {
    await assert.rejects(connection.command({ command: "setAuxOn", args } as Command), /INVALID_ARGUMENT/);
  }
  for (const command of [{ command: "toggleAux", args: { auxId: nextId } }, { command: "setAuxOn", args: { auxId: nextId, on: true } }] as const) {
    const requests = commands(server, "requestState").length;
    await assert.rejects(connection.command(command), /AUX_NOT_FOUND/);
    await until(server, "message", () => commands(server, "requestState").length === requests + 1);
    await until(connection, "status", () => connection.ready && !connection.resynchronizing);
  }
  assert.equal(server.snapshot.revision, restored.revision);
});

test("aux: ON → toggle OFF → toggle ON changes only the bound aux and keyUp sends nothing", async t => {
  const { server, host, settings } = await setupActions(t, "ko", { auxOn: true });
  const before = structuredClone(server.snapshot.state);
  host.appear("aux", { auxId: settings.auxId, auxName: settings.auxName, nameFallback: false }, 0, "aux");
  await key(host, "aux", 1, "#A78BFA", "ON");
  assert.equal(title(host, "aux"), settings.auxName);
  for (const on of [false, true]) {
    host.keyDown("aux"); host.keyUp("aux");
    await key(host, "aux", on ? 1 : 0, on ? "#A78BFA" : "#3A3F47", on ? "ON" : "OFF");
    assert.equal(host.visual("aux").image, keyImage(on ? "on" : "off", "ko", false, false, "aux"));
    assert.deepEqual(server.snapshot.state, { ...before, auxes: [{ ...before.auxes[0], on }, before.auxes[1]] });
  }
  await host.fence();
  assert.deepEqual(commands(server).map(m => ({ command: m.command, args: m.args })), [
    { command: "toggleAux", args: { auxId } }, { command: "toggleAux", args: { auxId } }
  ]);
  assert.ok(commands(server).every(m => m.ifRevision === undefined && m.sessionId === server.snapshot.sessionId)); host.assertBudget();
});

test("aux: explicit ON/OFF modes are idempotent and short titles preserve the saved binding", async t => {
  const { server, host, settings } = await setupActions(t, "en", { auxOn: true }), shortTitle = "Stage monitor output";
  host.appear("aux", { ...settings, mode: "on", shortTitle }, 0, "aux");
  await key(host, "aux", 1, "#A78BFA", "ON"); assert.equal(host.visual("aux").title, displayName(shortTitle));
  const revision = server.snapshot.revision, saved = host.contexts.get("aux")!.settings;
  for (const [i, mode] of ["on", "off", "off", "on"].entries()) {
    host.settings("aux", { ...saved, mode }); host.keyDown("aux"); host.keyUp("aux");
    await until(server, "command", () => commands(server).length === i + 1);
    await key(host, "aux", mode === "on" ? 1 : 0, mode === "on" ? "#A78BFA" : "#3A3F47", mode.toUpperCase());
    const ack = server.sent.find(m => m.type === "ack" && m.id === commands(server)[i].id);
    assert.equal(ack.changed, [false, true, false, true][i]); assert.deepEqual(ack.result, { on: mode === "on" });
    assert.equal(server.snapshot.revision, revision + [0, 1, 1, 2][i]!);
  }
  assert.deepEqual(commands(server).map(m => ({ command: m.command, args: m.args })),
    [true, false, false, true].map(on => ({ command: "setAuxOn", args: { auxId, on } })));
  assert.ok(commands(server).every(m => m.ifRevision === undefined));
  assert.equal(host.contexts.get("aux")!.settings.auxId, auxId); assert.equal(host.contexts.get("aux")!.settings.auxName, settings.auxName);
});

test("aux: mic/FX mute states never mute the aux lamp and stopped audio remains controllable", async t => {
  const { server, host, settings } = await setupActions(t, "ko", { auxOn: true });
  host.appear("aux", settings, 0, "aux"); await key(host, "aux", 1, "#A78BFA");
  server.mutate((s: Snapshot) => { s.state.muteGroups = { mic: true, fx: true }; s.state.muteAll = { mic: true, fx: true }; s.state.audio.running = false; });
  await host.wait(() => host.visual("aux").image === keyImage("on", "ko", true, false, "aux"));
  host.keyDown("aux"); host.keyUp("aux"); await key(host, "aux", 0, "#3A3F47", "OFF");
  assert.equal(host.visual("aux").image, keyImage("off", "ko", true, false, "aux"));
  assert.ok(svg(host.visual("aux").image).includes(strings.ko.audioStopped));
  assert.deepEqual(server.snapshot.state.muteGroups, { mic: true, fx: true }); assert.deepEqual(server.snapshot.state.muteAll, { mic: true, fx: true });
});

for (const language of ["ko", "en"] as const) test(`aux: absent capability requires an update while existing mic/FX keys still work (${language})`, async t => {
  const { server, host, settings } = await setupActions(t, language);
  assert.equal("auxes" in server.snapshot.state, false); assert.ok(!server.helloFixture.capabilities.includes("auxOn"));
  assert.deepEqual((validateServerMessage(server.snapshot) as Snapshot).state.auxes, []);
  host.appear("aux", { ...settings, auxId, auxName: "Monitor" }, 0, "aux");
  host.appear("mic", settings, 1); host.appear("fx", settings, 2, "fx");
  await host.wait(() => host.visual("aux").title === statusTitle(strings[language].unsupported));
  assert.equal(host.visual("aux").state, 0); assert.equal(host.visual("aux").image, keyImage("missing", language));
  host.keyDown("aux"); host.keyUp("aux"); await host.wait(() => alerts(host, "aux").length === 1);
  await key(host, "mic", 1, "#35D07F"); await key(host, "fx", 1, "#4C8DFF");
  host.keyDown("mic"); await key(host, "mic", 0, "#3A3F47"); host.keyDown("fx"); await key(host, "fx", 0, "#3A3F47");
  await host.fence(); assert.deepEqual(commands(server).map(m => m.command), ["toggleChannel", "toggleFx"]);
});

test("aux: a missing saved ID displays missing and sends no command", async t => {
  const { server, host, settings } = await setupActions(t, "en", { auxOn: true });
  host.appear("aux", { ...settings, auxId: nextId, nameFallback: false }, 0, "aux");
  await host.wait(() => host.visual("aux").title === statusTitle(strings.en.missingAux));
  host.keyDown("aux"); host.keyUp("aux"); await host.wait(() => alerts(host, "aux").length === 1);
  const { pi } = await options(host, "aux"); assert.equal(pi.messages.at(-1)!.payload.message, strings.en.missingAux);
  assert.equal(commands(server).length, 0);
});

test("aux: name/ON deltas update the key and PI; a new session rebinds by name without replacing the saved ID", async t => {
  const { server, host, settings } = await setupActions(t, "ko", { auxOn: true });
  host.appear("aux", settings, 0, "aux"); await key(host, "aux", 1, "#A78BFA");
  const { pi } = await options(host, "aux"), name = "무대 모니터";
  assert.deepEqual(pi.messages.at(-1)!.payload.auxes, server.snapshot.state.auxes.map(({ id, name }: { id: string; name: string }) => ({ id, name })));
  server.mutate((s: Snapshot) => { s.state.auxes[0]!.name = name; });
  await until(host, "pi", () => pi.messages.some(m => m.event === "didReceiveSettings" && m.payload.settings.auxName === name));
  await host.wait(() => title(host, "aux") === name);
  assert.deepEqual(server.sent.filter(m => m.type === "stateDelta").at(-1).changes, { auxes: [{ id: auxId, name }] });
  assert.equal(host.visual("aux").state, 1);
  server.mutate((s: Snapshot) => { s.state.auxes[0]!.on = false; });
  await key(host, "aux", 0, "#3A3F47", "OFF"); assert.equal(title(host, "aux"), name);
  assert.deepEqual(server.sent.filter(m => m.type === "stateDelta").at(-1).changes, { auxes: [{ id: auxId, on: false }] });
  await until(host, "pi", () => pi.messages.at(-1)?.payload.revision === server.snapshot.revision);
  assert.equal(pi.messages.at(-1)!.payload.auxes[0].name, name);
  host.keyDown("aux"); await key(host, "aux", 1, "#A78BFA");
  server.mutate((s: Snapshot) => { s.sessionId = "dddddddddddd4ddd8ddddddddddddddd"; s.state.auxes[0] = { id: nextId, name, on: false }; }, true);
  await key(host, "aux", 0, "#3A3F47");
  host.keyDown("aux"); await key(host, "aux", 1, "#A78BFA");
  assert.deepEqual(commands(server).map(m => m.args), [{ auxId }, { auxId: nextId }]);
  assert.equal(host.contexts.get("aux")!.settings.auxId, auxId); assert.equal(host.contexts.get("aux")!.settings.auxName, name);
});

test("aux: duplicate fallback names require selection in a new session", async t => {
  const { server, host, settings } = await setupActions(t, "ko", { auxOn: true });
  host.appear("aux", settings, 0, "aux"); await key(host, "aux", 1, "#A78BFA");
  server.mutate((s: Snapshot) => {
    s.sessionId = "dddddddddddd4ddd8ddddddddddddddd"; s.state.auxes[0]!.id = nextId; s.state.auxes[1]!.name = settings.auxName;
  }, true);
  await host.wait(() => host.visual("aux").title === statusTitle(strings.ko.duplicate));
  host.keyDown("aux"); await host.wait(() => alerts(host, "aux").length === 1); assert.equal(commands(server).length, 0);
});

test("aux: deletion in a full state shows missing without rebinding to a same-name replacement", async t => {
  const { server, host, settings } = await setupActions(t, "ko", { auxOn: true });
  host.appear("aux", settings, 0, "aux"); await key(host, "aux", 1, "#A78BFA");
  server.mutate((s: Snapshot) => { s.state.auxes.shift(); s.state.auxes[0]!.name = settings.auxName; }, true);
  await host.wait(() => host.visual("aux").title === statusTitle(strings.ko.missingAux));
  assert.ok(svg(host.visual("aux").image).includes("stroke-dasharray"));
  host.keyDown("aux"); host.keyUp("aux"); await host.wait(() => alerts(host, "aux").length === 1);
  assert.equal(commands(server).length, 0); assert.equal(server.snapshot.state.auxes[0].on, false);
});
