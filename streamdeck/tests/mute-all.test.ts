import test from "node:test";
import assert from "node:assert/strict";
import { until } from "./helpers.mjs";
import { validateServerMessage, type Snapshot } from "../src/livemix/protocol.js";
import { actionImage, displayName, keyImage, sendImage, statusTitle } from "../src/ui/key-renderer.js";
import { strings } from "../src/ui/strings.js";
import { setupActions, key, commands, alerts, title } from "./action-helpers.js";

for (const group of ["mic", "fx"] as const) test(`${group} mute-all: two toggles, explicit mute/unmute, no-op, title and stopped audio`, async t => {
  const { server, host } = await setupActions(t), kind = `${group}-mute-all`, count = group === "mic" ? 3 : 2;
  const before = structuredClone(server.snapshot.state);
  host.appear("all", { mode: "toggle" }, 0, kind);
  await key(host, "all", 0, "#35D07F", `전체 ${count}개`);
  assert.equal(title(host, "all"), strings.ko[group === "mic" ? "micMuteAll" : "fxMuteAll"]);
  for (const muted of [true, false]) {
    host.keyDown("all"); host.keyUp("all");
    await host.wait(() => host.visual("all").state === (muted ? 1 : 0)
      && host.visual("all").image === actionImage(muted ? "mute-every-muted" : "mute-every-clear", "ko", count, 0, false, group));
    assert.equal(server.snapshot.state.muteAll[group], muted);
  }
  assert.deepEqual(commands(server).map(m => ({ command: m.command, args: m.args })), [
    { command: "toggleMuteAll", args: { group } }, { command: "toggleMuteAll", args: { group } }
  ]);
  const revision = server.snapshot.revision, shortTitle = "방송 전체 음소거 스위치";
  server.mutate((s: Snapshot) => { s.state.audio.running = false; });
  for (const [i, mode] of ["mute", "mute", "unmute"].entries()) {
    host.settings("all", { ...host.contexts.get("all")!.settings, mode, shortTitle });
    host.keyDown("all"); host.keyUp("all");
    await until(server, "command", () => commands(server).length === i + 3);
    await host.wait(() => host.visual("all").state === (mode === "mute" ? 1 : 0)
      && host.visual("all").image === actionImage(mode === "mute" ? "mute-every-muted" : "mute-every-clear", "ko", count, 0, true, group)
      && host.visual("all").title === displayName(shortTitle));
    assert.equal(server.snapshot.revision, revision + [2, 2, 3][i]!);
  }
  await host.fence();
  assert.deepEqual(commands(server, "setMuteAll").map(m => m.args), [true, true, false].map(muted => ({ group, muted })));
  assert.equal(commands(server).length, 5, "Key release sends nothing");
  assert.ok(commands(server).every(m => m.ifRevision === undefined && m.instanceId === server.snapshot.instanceId && m.sessionId === server.snapshot.sessionId));
  assert.deepEqual(server.snapshot.state, { ...before, audio: { running: false } }); host.assertBudget();
});

test("mute-all: unmarked mic/FX and both send controls follow either mute, keeping switches and amounts", async t => {
  const { server, host } = await setupActions(t), mic = server.snapshot.state.channels[1], fx = server.snapshot.state.fx[1];
  server.mutate((s: Snapshot) => { s.state.channels[1]!.sends.push({ fxId: fx.id, amount: 0.35, pre: false }); });
  const settings = { channelId: mic.id, channelName: mic.name, fxId: fx.id, fxName: fx.name };
  host.appear("mic", settings); host.appear("fx", settings, 1, "fx");
  host.appear("send", { ...settings, mode: "up", stepPercent: 5 }, 2, "fx-send-step"); host.appearDial("dial", settings);
  await key(host, "mic", 1, "#35D07F"); await key(host, "fx", 1, "#4C8DFF");
  const before = structuredClone(server.snapshot.state);
  for (const group of ["mic", "fx"] as const) for (const muted of [true, false]) {
    server.mutate((s: Snapshot) => { s.state.muteAll[group] = muted; });
    await host.wait(() => host.visual(group).image === keyImage(muted ? "muted-on" : "on", "ko", false, false, group));
    await host.wait(() => host.visual("send").image === sendImage(35, "+5", "ko", muted));
    await host.wait(() => host.feedback("dial").status === (muted ? strings.ko.muteApplied : ""));
    assert.equal(host.visual("mic").state, 1); assert.equal(host.visual("fx").state, 1); assert.equal(host.visual("send").state, 0);
    const other = group === "mic" ? "fx" : "mic";
    assert.equal(host.visual(other).image, keyImage("on", "ko", false, false, other));
  }
  // Own OFF switches remain OFF while the mute overlay is shown.
  server.mutate((s: Snapshot) => { s.state.channels[1]!.on = false; s.state.fx[1]!.on = false; s.state.muteAll = { mic: true, fx: true }; });
  for (const group of ["mic", "fx"] as const) {
    await host.wait(() => host.visual(group).image === keyImage("muted-off", "ko", false, false, group));
    assert.equal(host.visual(group).state, 0);
  }
  assert.deepEqual(server.snapshot.state.channels.map((c: any) => c.sends), before.channels.map((c: any) => c.sends));
  assert.deepEqual(server.snapshot.state.muteGroups, before.muteGroups); assert.equal(commands(server).length, 0); host.assertBudget();
});

test("mute-all and mute group are independent: releasing all-mute leaves only marked mics muted", async t => {
  const { server, host, settings } = await setupActions(t), outside = server.snapshot.state.channels[1];
  server.mutate((s: Snapshot) => { s.state.channels = s.state.channels.slice(0, 2); }, true);
  host.appear("marked", settings); host.appear("outside", { channelId: outside.id, channelName: outside.name }, 1);
  host.appear("group", {}, 2, "mic-mute-group"); host.appear("all", {}, 3, "mic-mute-all");
  await key(host, "group", 0, "#35D07F"); await key(host, "all", 0, "#35D07F");
  host.keyDown("group"); await key(host, "group", 1, "#FF5A5F"); await key(host, "marked", 1, "#FF5A5F");
  const groupImage = host.visual("group").image;
  host.keyDown("all"); await key(host, "all", 1, "#FF5A5F"); await key(host, "outside", 1, "#FF5A5F");
  assert.equal(host.visual("group").image, groupImage);
  // Group release/re-mute must not release all-mute.
  for (const muted of [false, true]) {
    host.keyDown("group"); await key(host, "group", muted ? 1 : 0, muted ? "#FF5A5F" : "#35D07F");
    assert.equal(server.snapshot.state.muteAll.mic, true);
    assert.equal(host.visual("marked").image, keyImage("muted-on", "ko"));
    assert.equal(host.visual("outside").image, keyImage("muted-on", "ko"));
  }
  host.keyDown("all"); await key(host, "all", 0, "#35D07F");
  await host.wait(() => host.visual("outside").image === keyImage("on", "ko"));
  assert.equal(host.visual("marked").image, keyImage("muted-on", "ko"));
  assert.equal(host.visual("group").image, groupImage);
  assert.deepEqual(server.snapshot.state.muteGroups, { mic: true, fx: false }); host.assertBudget();
});

for (const language of ["ko", "en"] as const) test(`mute-all: missing capability alerts once per new key; existing mic still works (${language})`, async t => {
  const { server, host, settings } = await setupActions(t, language, { muteAll: false });
  for (const [i, group] of ["mic", "fx"].entries()) {
    host.appear(group, {}, i, `${group}-mute-all`);
    await host.wait(() => host.visual(group).title === statusTitle(strings[language].unsupported));
    host.keyDown(group); host.keyUp(group); await host.wait(() => alerts(host, group).length === 1);
    assert.equal(host.visual(group).state, 0);
  }
  await host.fence(); assert.equal(commands(server).length, 0);
  host.appear("existing", settings, 2); await key(host, "existing", 1, "#35D07F");
  host.keyDown("existing"); host.keyUp("existing"); await key(host, "existing", 0, "#3A3F47");
  assert.deepEqual(commands(server).map(m => m.command), ["toggleChannel"]);
});

test("LiveMix 0.17.x omits muteAll: defaults are false and existing mic/group/FX keys work", async t => {
  const { server, host, settings } = await setupActions(t, "en", { legacyMuteAll: true });
  assert.ok(!("muteAll" in server.snapshot.state)); assert.ok(!server.helloFixture.capabilities.includes("muteAll"));
  assert.deepEqual((validateServerMessage(server.snapshot) as Snapshot).state.muteAll, { mic: false, fx: false });
  host.appear("mic", settings); host.appear("group", {}, 1, "mic-mute-group"); host.appear("fx", settings, 2, "fx");
  await key(host, "mic", 1, "#35D07F"); await key(host, "fx", 1, "#4C8DFF"); await key(host, "group", 0, "#35D07F");
  host.keyDown("group"); await key(host, "group", 1, "#FF5A5F"); await key(host, "mic", 1, "#FF5A5F");
  host.keyDown("mic"); await key(host, "mic", 0, "#FF5A5F", "Originally OFF");
  host.keyDown("fx"); await key(host, "fx", 0, "#3A3F47", "OFF");
  assert.deepEqual(commands(server).map(m => m.command), ["toggleMuteGroup", "toggleChannel", "toggleFx"]);
});
