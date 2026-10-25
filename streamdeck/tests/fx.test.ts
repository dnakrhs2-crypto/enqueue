import test from "node:test";
import assert from "node:assert/strict";
import { until } from "./helpers.mjs";
import { validateServerMessage, type Snapshot } from "../src/livemix/protocol.js";
import { displayName, keyImage, statusTitle } from "../src/ui/key-renderer.js";
import { strings } from "../src/ui/strings.js";
import { setupActions, key, svg, title, commands, alerts, options } from "./action-helpers.js";

test("fx: ON → toggle OFF → toggle ON uses only the bound FX and keyUp sends nothing", async t => {
  const { server, host, settings } = await setupActions(t);
  const before = structuredClone(server.snapshot.state);
  host.appear("fx", { fxId: settings.fxId, fxName: settings.fxName, nameFallback: false }, 0, "fx");
  await key(host, "fx", 1, "#4C8DFF", "ON");
  assert.equal(host.visual("fx").image, keyImage("on", "ko", false, false, "fx"));
  assert.equal(title(host, "fx"), settings.fxName);
  for (const on of [false, true]) {
    host.keyDown("fx"); host.keyUp("fx");
    await key(host, "fx", on ? 1 : 0, on ? "#4C8DFF" : "#3A3F47", on ? "ON" : "OFF");
    assert.equal(host.visual("fx").image, keyImage(on ? "on" : "off", "ko", false, false, "fx"));
    assert.equal(server.snapshot.state.fx[0].on, on);
  }
  await host.fence();
  assert.deepEqual(commands(server).map(m => ({ command: m.command, args: m.args })), [
    { command: "toggleFx", args: { fxId: settings.fxId } }, { command: "toggleFx", args: { fxId: settings.fxId } }
  ]);
  assert.ok(commands(server).every(m => m.ifRevision === undefined && m.sessionId === server.snapshot.sessionId));
  assert.deepEqual(server.snapshot.state, before); host.assertBudget();
});

test("fx: explicit ON/OFF modes are idempotent and display names stay independent of bindings", async t => {
  const { server, host, settings } = await setupActions(t, "en"), shortTitle = "Octave doubler send";
  host.appear("fx", { ...settings, mode: "on", shortTitle }, 0, "fx");
  await key(host, "fx", 1, "#4C8DFF", "ON");
  assert.equal(host.visual("fx").title, displayName(shortTitle));
  const initial = server.snapshot.revision, saved = host.contexts.get("fx")!.settings;
  for (const [i, mode] of ["on", "off", "off", "on"].entries()) {
    // A PI mode edit preserves the migrated settings already saved by the plugin.
    host.settings("fx", { ...saved, mode }); host.keyDown("fx"); host.keyUp("fx");
    await until(server, "command", () => commands(server).length === i + 1);
    await key(host, "fx", mode === "on" ? 1 : 0, mode === "on" ? "#4C8DFF" : "#3A3F47", mode.toUpperCase());
    assert.equal(server.snapshot.revision, initial + [0, 1, 1, 2][i]!);
  }
  assert.deepEqual(commands(server).map(m => ({ command: m.command, args: m.args })),
    [true, false, false, true].map(on => ({ command: "setFxOn", args: { fxId: settings.fxId, on } })));
  assert.ok(commands(server).every(m => m.ifRevision === undefined));
  assert.equal(host.contexts.get("fx")!.settings.fxName, settings.fxName);
});

test("fx: group mute shows original ON/OFF, nonmembers stay plain, stopped audio remains controllable", async t => {
  const { server, host, settings } = await setupActions(t);
  const sends = structuredClone(server.snapshot.state.channels.map((c: any) => c.sends));
  const outside = server.snapshot.state.fx[1];
  host.appear("fx", settings, 0, "fx");
  host.appear("outside", { fxId: outside.id, fxName: outside.name }, 1, "fx");
  await key(host, "fx", 1, "#4C8DFF", "ON");
  server.mutate((s: Snapshot) => { s.state.muteGroups.fx = true; s.state.audio.running = false; });
  await key(host, "fx", 1, "#FF5A5F", "원래 ON");
  await host.wait(() => host.visual("outside").image === keyImage("on", "ko", true, false, "fx"));
  assert.equal(host.visual("fx").image, keyImage("muted-on", "ko", true, false, "fx"));
  host.keyDown("fx"); host.keyUp("fx");
  await key(host, "fx", 0, "#FF5A5F", "원래 OFF");
  assert.equal(host.visual("fx").image, keyImage("muted-off", "ko", true, false, "fx"));
  assert.ok(svg(host.visual("fx").image).includes("오디오 멈춤"));
  assert.equal(host.visual("outside").state, 1);
  assert.deepEqual(server.snapshot.state.channels.map((c: any) => c.sends), sends);
  assert.equal(server.snapshot.state.muteGroups.fx, true);
  server.mutate((s: Snapshot) => { s.state.muteGroups.fx = false; });
  await key(host, "fx", 0, "#3A3F47", "OFF");
  assert.equal(server.snapshot.state.fx[0].on, false);
});

for (const language of ["ko", "en"] as const) test(`fx: absent capability displays update text and alerts once without sending (${language})`, async t => {
  const { server, host, settings } = await setupActions(t, language, { fxOn: false });
  host.appear("fx", settings, 0, "fx");
  await host.wait(() => host.visual("fx").title === statusTitle(strings[language].unsupported));
  assert.equal(host.visual("fx").state, 0);
  host.keyDown("fx"); host.keyUp("fx"); await host.wait(() => alerts(host, "fx").length === 1);
  await host.fence(); assert.equal(commands(server).length, 0);
});

test("fx: deleting the selected FX displays missing and never edits a same-name replacement", async t => {
  const { server, host, settings } = await setupActions(t);
  host.appear("fx", settings, 0, "fx"); await key(host, "fx", 1, "#4C8DFF");
  server.mutate((s: Snapshot) => {
    s.state.fx = s.state.fx.filter(f => f.id !== settings.fxId); s.state.fx[0]!.name = settings.fxName;
    for (const channel of s.state.channels) channel.sends = channel.sends.filter(send => send.fxId !== settings.fxId);
  }, true);
  await host.wait(() => title(host, "fx") === "FX 채널 없음");
  assert.ok(svg(host.visual("fx").image).includes("stroke-dasharray"));
  host.keyDown("fx"); host.keyUp("fx"); await host.wait(() => alerts(host, "fx").length === 1);
  assert.equal(commands(server).length, 0);
});

test("fx: rename persists the fallback name; a new session binds its unique matching FX without replacing the saved UUID", async t => {
  const { server, host, settings } = await setupActions(t);
  host.appear("fx", settings, 0, "fx"); await key(host, "fx", 1, "#4C8DFF");
  const { pi } = await options(host, "fx"), name = "옥타브 더블러", nextId = "cccccccccccc4ccc8ccccccccccccccc";
  server.mutate((s: Snapshot) => { s.state.fx[0]!.name = name; });
  await until(host, "pi", () => pi.messages.some(m => m.event === "didReceiveSettings" && m.payload.settings.fxName === name));
  await host.wait(() => title(host, "fx") === name);
  assert.equal(host.contexts.get("fx")!.settings.fxId, settings.fxId);
  host.keyDown("fx"); await key(host, "fx", 0, "#3A3F47", "OFF");
  server.mutate((s: Snapshot) => {
    s.sessionId = "dddddddddddd4ddd8ddddddddddddddd";
    s.state.fx[0] = { ...s.state.fx[0]!, id: nextId, on: true };
    for (const channel of s.state.channels) for (const send of channel.sends) if (send.fxId === settings.fxId) send.fxId = nextId;
  }, true);
  await key(host, "fx", 1, "#4C8DFF", "ON");
  host.keyDown("fx"); await key(host, "fx", 0, "#3A3F47", "OFF");
  assert.deepEqual(commands(server).map(m => m.args), [{ fxId: settings.fxId }, { fxId: nextId }]);
  assert.equal(host.contexts.get("fx")!.settings.fxId, settings.fxId);
  assert.equal(host.contexts.get("fx")!.settings.fxName, name);
});

test("fx: duplicate names in a new session require selection and send no command", async t => {
  const { server, host, settings } = await setupActions(t);
  host.appear("fx", settings, 0, "fx"); await key(host, "fx", 1, "#4C8DFF");
  server.mutate((s: Snapshot) => {
    s.sessionId = "dddddddddddd4ddd8ddddddddddddddd"; s.state.fx[0]!.id = "cccccccccccc4ccc8ccccccccccccccc";
    s.state.fx[1]!.name = settings.fxName;
    for (const channel of s.state.channels) channel.sends = [];
  }, true);
  await host.wait(() => host.visual("fx").title === statusTitle(strings.ko.duplicate));
  host.keyDown("fx"); host.keyUp("fx"); await host.wait(() => alerts(host, "fx").length === 1);
  assert.equal(commands(server).length, 0);
});

test("mic and fx keys share a connection and redraw from independent target deltas", async t => {
  const { server, host, settings } = await setupActions(t);
  host.appear("mic", settings); host.appear("fx", settings, 1, "fx");
  await key(host, "mic", 1, "#35D07F"); await key(host, "fx", 1, "#4C8DFF");
  const micImage = host.visual("mic").image, fxImage = host.visual("fx").image;
  assert.notEqual(micImage, fxImage);
  host.keyDown("fx"); await key(host, "fx", 0, "#3A3F47", "OFF");
  assert.equal(server.snapshot.state.channels[0].on, true); assert.equal(host.visual("mic").image, micImage);
  const fxOff = host.visual("fx").image;
  host.keyDown("mic"); await key(host, "mic", 0, "#3A3F47", "OFF");
  assert.equal(server.snapshot.state.fx[0].on, false); assert.equal(host.visual("fx").image, fxOff);
  server.mutate((s: Snapshot) => { s.state.channels[0]!.on = true; s.state.fx[0]!.on = true; });
  await key(host, "mic", 1, "#35D07F", "ON"); await key(host, "fx", 1, "#4C8DFF", "ON");
  assert.deepEqual(commands(server).map(m => ({ command: m.command, args: m.args })), [
    { command: "toggleFx", args: { fxId: settings.fxId } }, { command: "toggleChannel", args: { channelId: settings.channelId } }
  ]);
  assert.equal(server.connections, 1); host.assertBudget();
});

test("LiveMix 0.15.5 FX snapshots default to ON and existing mic keys still work", async t => {
  const { server, host, settings } = await setupActions(t, "en", { legacyFx: true });
  assert.ok(server.snapshot.state.fx.every((fx: object) => !("on" in fx)));
  assert.ok(!server.helloFixture.capabilities.includes("fxOn"));
  assert.ok((validateServerMessage(server.snapshot) as Snapshot).state.fx.every(fx => fx.on));
  host.appear("mic", settings); host.appear("fx", settings, 1, "fx");
  await key(host, "mic", 1, "#35D07F", "ON");
  await host.wait(() => host.visual("fx").title === statusTitle(strings.en.unsupported));
  host.keyDown("mic"); host.keyUp("mic"); await key(host, "mic", 0, "#3A3F47", "OFF");
  assert.deepEqual(commands(server).map(m => m.command), ["toggleChannel"]);
});
