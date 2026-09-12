import test from "node:test";
import assert from "node:assert/strict";
import { setTimeout as delay } from "node:timers/promises";
import { until } from "./helpers.mjs";
import type { Snapshot } from "../src/livemix/protocol.js";
import { actionImage, statusTitle } from "../src/ui/key-renderer.js";
import { strings } from "../src/ui/strings.js";
import { setupActions, key, svg, title, commands, alerts, options, barrier } from "./action-helpers.js";

test("all-mics: mixed 2/3 → all OFF → all ON; two keys sync and mute groups stay untouched", async t => {
  const { server, host, settings } = await setupActions(t);
  server.mutate((s: Snapshot) => { s.state.muteGroups = { mic: true, fx: true }; });
  host.appear("a", settings, 0, "all-mics"); host.appear("b", settings, 1, "all-mics");
  await key(host, "a", 0, "#FFB454", "2/3"); await key(host, "b", 0, "#FFB454", "일부 ON");
  assert.equal(title(host, "a"), "전체 마이크");
  host.keyDown("a"); host.keyUp("a");
  for (const id of ["a", "b"]) await key(host, id, 0, "#3A3F47", "모두 OFF");
  assert.ok(server.snapshot.state.channels.every((c: any) => !c.on));
  host.keyDown("b"); host.keyUp("b");
  for (const id of ["a", "b"]) await key(host, id, 1, "#35D07F", "3/3");
  assert.deepEqual(server.snapshot.state.muteGroups, { mic: true, fx: true });
  assert.deepEqual(commands(server).map(m => m.args.on), [false, true]);
  assert.ok(commands(server).every(m => Number.isInteger(m.ifRevision))); assert.equal(server.connections, 1);
  for (const mode of ["off", "on"]) {
    host.settings("a", { ...settings, mode }); host.keyDown("a"); host.keyUp("a");
    await key(host, "a", mode === "on" ? 1 : 0, mode === "on" ? "#35D07F" : "#3A3F47", mode === "on" ? "모두 ON" : "모두 OFF");
  }
  assert.deepEqual(commands(server).map(m => m.args.on), [false, true, false, true]);
});

for (const group of ["mic", "fx"] as const) test(`${group} mute group: toggle/mute/unmute, member count, two keys, zero-member latch`, async t => {
  const { server, host, settings } = await setupActions(t), kind = `${group}-mute-group`;
  const before = structuredClone(server.snapshot.state.channels.map((c: any) => c.sends));
  host.appear("a", settings, 0, kind); host.appear("b", settings, 1, kind);
  await key(host, "a", 0, "#35D07F", group === "mic" ? "대상 2개" : "대상 1개");
  host.keyDown("a"); host.keyUp("a");
  for (const id of ["a", "b"]) await key(host, id, 1, "#FF5A5F", "뮤트");
  assert.equal(server.snapshot.state.muteGroups[group], true); assert.equal(commands(server).length, 1);
  assert.equal(commands(server)[0].command, "toggleMuteGroup");
  host.settings("a", { ...settings, mode: "mute" }); host.keyDown("a");
  await until(server, "command", () => commands(server).length === 2);
  host.settings("a", { ...settings, mode: "unmute" }); host.keyDown("a");
  for (const id of ["a", "b"]) await key(host, id, 0, "#35D07F", "해제");
  assert.deepEqual(commands(server, "setMuteGroup").map(m => m.args), [{ group, muted: true }, { group, muted: false }]);
  server.mutate((s: Snapshot) => { for (const item of group === "mic" ? s.state.channels : s.state.fx) item.muteGroup = false; });
  await key(host, "b", 0, "#35D07F", "대상 0개"); host.keyDown("b"); host.keyUp("b");
  await key(host, "b", 1, "#FF5A5F", "대상 0개"); assert.equal(server.snapshot.state.muteGroups[group], true);
  assert.deepEqual(server.snapshot.state.channels.map((c: any) => c.sends), before);
});

test("plugin group: numbered title, inverse off/state, explicit modes, shared target and vanished slot", async t => {
  const { server, host, settings } = await setupActions(t);
  host.appear("a", settings, 0, "plugin-group"); host.appear("b", settings, 1, "plugin-group");
  await key(host, "a", 1, "#4C8DFF", "ON");
  assert.equal(title(host, "a"), "마이크 1 · 그룹 1");
  host.keyDown("a"); host.keyUp("a");
  for (const id of ["a", "b"]) await key(host, id, 0, "#FF5A5F", "OFF");
  assert.equal(server.snapshot.state.channels[0].pluginGroups[0].off, true);
  assert.equal(server.snapshot.state.channels[0].pluginGroups[1].off, true, "Other slots remain independent");
  for (const mode of ["on", "off"]) {
    host.settings("a", { ...settings, mode }); host.keyDown("a"); host.keyUp("a");
    await key(host, "a", mode === "on" ? 1 : 0, mode === "on" ? "#4C8DFF" : "#FF5A5F", mode.toUpperCase());
  }
  assert.deepEqual(commands(server).map(m => m.args.off), [true, false, true]);
  server.mutate((s: Snapshot) => { s.state.channels[0]!.pluginGroups = []; }, true);
  await host.wait(() => title(host, "a") === "그룹 없음");
  assert.ok(svg(host.visual("a").image).includes("stroke-dasharray"));
  host.keyDown("a"); host.keyUp("a"); await host.wait(() => alerts(host, "a").length === 1);
  assert.equal(commands(server).length, 3);
});

test("plugin-group-all: all ON 3/3 → all OFF → all ON, two keys sync without channel bindings", async t => {
  const { server, host } = await setupActions(t), settings = { groupIndex: 1, mode: "toggle", shortTitle: "" };
  const unchanged = () => ({ channels: server.snapshot.state.channels.map((c: any) => ({ ...c, pluginGroups: c.pluginGroups.filter((g: any) => g.index !== 1) })), muteGroups: server.snapshot.state.muteGroups });
  const before = structuredClone(unchanged());
  host.appear("a", settings, 0, "plugin-group-all"); host.appear("b", settings, 1, "plugin-group-all");
  for (const id of ["a", "b"]) {
    await key(host, id, 1, "#4C8DFF", "모두 ON"); assert.ok(svg(host.visual(id).image).includes("3/3"));
    assert.equal(title(host, id), "전체 마이크 · 그룹 1");
  }
  host.keyDown("a"); host.keyUp("a");
  for (const id of ["a", "b"]) { await key(host, id, 0, "#FF5A5F", "모두 OFF"); assert.ok(svg(host.visual(id).image).includes("0/3")); }
  assert.ok(server.snapshot.state.channels.every((c: any) => c.pluginGroups[0].off));
  host.keyDown("b"); host.keyUp("b");
  for (const id of ["a", "b"]) await key(host, id, 1, "#4C8DFF", "3/3");
  assert.ok(server.snapshot.state.channels.every((c: any) => !c.pluginGroups[0].off));
  assert.deepEqual(commands(server).map(m => ({ command: m.command, args: m.args })), [
    { command: "setPluginGroupOffEverywhere", args: { index: 1, off: true } },
    { command: "setPluginGroupOffEverywhere", args: { index: 1, off: false } }
  ]);
  assert.ok(commands(server).every(m => Number.isInteger(m.ifRevision) && m.sessionId === server.snapshot.sessionId));
  assert.deepEqual(unchanged(), before); assert.equal(server.connections, 1);
});

test("plugin-group-all: external partial edit shows Some ON 1/3, toggle turns every target off", async t => {
  const { server, host } = await setupActions(t);
  server.mutate((s: Snapshot) => { for (const c of s.state.channels) c.pluginGroups[0]!.off = true; });
  host.appear("a", { groupIndex: 1 }, 0, "plugin-group-all"); await key(host, "a", 0, "#FF5A5F", "모두 OFF");
  server.mutate((s: Snapshot) => { s.state.channels[1]!.pluginGroups[0]!.off = false; s.state.audio.running = false; });
  await key(host, "a", 0, "#FFB454", "일부 ON");
  assert.equal(host.visual("a").image, actionImage("plugin-all-mixed", "ko", 1, 3, true, "mic", 1));
  host.keyDown("a"); host.keyUp("a"); await key(host, "a", 0, "#FF5A5F", "모두 OFF");
  assert.ok(svg(host.visual("a").image).includes("오디오 멈춤"));
  assert.deepEqual(commands(server).map(m => m.args), [{ index: 1, off: true }]);
  assert.ok(server.snapshot.state.channels.every((c: any) => c.pluginGroups[0].off));
});

test("plugin-group-all: explicit ON/OFF modes are idempotent and display aliases stay independent", async t => {
  const { server, host } = await setupActions(t, "en");
  host.appear("a", { groupIndex: 1, mode: "on", shortTitle: "Voice chain" }, 0, "plugin-group-all");
  await key(host, "a", 1, "#4C8DFF", "All ON"); assert.equal(title(host, "a"), "Voice chain");
  const initial = server.snapshot.revision;
  for (const [i, mode] of ["on", "off", "off", "on"].entries()) {
    host.settings("a", { groupIndex: 1, mode }); host.keyDown("a"); host.keyUp("a");
    await until(server, "command", () => commands(server).length === i + 1);
    await key(host, "a", mode === "on" ? 1 : 0, mode === "on" ? "#4C8DFF" : "#FF5A5F", mode === "on" ? "All ON" : "All OFF");
    assert.equal(server.snapshot.revision, initial + [0, 1, 1, 2][i]!);
    assert.ok(server.snapshot.state.channels.every((c: any) => c.pluginGroups[0].off === (mode === "off")));
  }
  assert.equal(title(host, "a"), "All mics · Group 1");
  assert.deepEqual(commands(server).map(m => m.args), [false, true, true, false].map(off => ({ index: 1, off })));
});

test("plugin-group-all: counts include only existing slots, absent and vanished groups alert without commands", async t => {
  const { server, host } = await setupActions(t);
  server.mutate((s: Snapshot) => { s.state.channels[2]!.pluginGroups = s.state.channels[2]!.pluginGroups.filter(g => g.index !== 2); }, true);
  host.appear("a", { groupIndex: 2 }, 0, "plugin-group-all"); await key(host, "a", 0, "#FF5A5F", "0/2");
  host.keyDown("a"); await key(host, "a", 1, "#4C8DFF", "2/2");
  assert.equal(host.visual("a").image, actionImage("plugin-all-on", "ko", 2, 2, false, "mic", 2));
  assert.equal(title(host, "a"), "전체 마이크 · 그룹 2");
  assert.equal(server.snapshot.state.channels[2].pluginGroups.length, 1);
  host.appear("missing", { groupIndex: 3 }, 1, "plugin-group-all");
  await host.wait(() => title(host, "missing") === "그룹 없음");
  assert.ok(svg(host.visual("missing").image).includes("stroke-dasharray"));
  host.keyDown("missing"); host.keyUp("missing"); await host.wait(() => alerts(host, "missing").length === 1);
  server.mutate((s: Snapshot) => { for (const c of s.state.channels) c.pluginGroups = c.pluginGroups.filter(g => g.index !== 2); }, true);
  await host.wait(() => title(host, "a") === "그룹 없음");
  host.keyDown("a"); host.keyUp("a"); await host.wait(() => alerts(host, "a").length === 1);
  assert.equal(commands(server).length, 1);
});

for (const language of ["ko", "en"] as const) test(`plugin-group-all: missing capability shows the update message and never sends (${language})`, async t => {
  const { server, host } = await setupActions(t, language, { pluginGroupsEverywhere: false });
  host.appear("a", { groupIndex: 1 }, 0, "plugin-group-all");
  await host.wait(() => host.visual("a").title === statusTitle(strings[language].unsupported));
  assert.equal(host.visual("a").state, 0);
  host.keyDown("a"); host.keyUp("a"); await host.wait(() => alerts(host, "a").length === 1);
  assert.equal(commands(server).length, 0);
});

test("plugin-group-all: two keys serialize intentions behind ACK plus canonical state", async t => {
  const { server, host } = await setupActions(t);
  for (const [i, id] of ["a", "b"].entries()) host.appear(id, { groupIndex: 1 }, i, "plugin-group-all");
  for (const id of ["a", "b"]) await key(host, id, 1, "#4C8DFF", "3/3");
  server.behavior.omitDelta = true; host.keyDown("a");
  await until(server, "command", () => commands(server).length === 1);
  host.keyDown("b"); await barrier(host, "b");
  assert.equal(commands(server).length, 1);
  for (const id of ["a", "b"]) assert.equal(host.visual(id).image, actionImage("plugin-all-on", "ko", 3, 3));
  server.behavior.omitDelta = false; server.publishDelta();
  await until(server, "command", () => commands(server).length === 2);
  for (const id of ["a", "b"]) await key(host, id, 1, "#4C8DFF", "3/3");
  assert.deepEqual(commands(server).map(m => [m.args.off, m.ifRevision]), [[true, 1], [false, 2]]);
});

for (const kind of ["all-mics", "plugin-group", "plugin-group-all"]) test(`${kind}: conflict refreshes state before recomputing the intended toggle`, async t => {
  const { server, host, settings } = await setupActions(t), command = kind === "all-mics" ? "setAllChannelsOn" : kind === "plugin-group-all" ? "setPluginGroupOffEverywhere" : "setPluginGroupOff";
  host.appear("a", settings, 0, kind);
  await key(host, "a", kind === "all-mics" ? 0 : 1, kind === "all-mics" ? "#FFB454" : "#4C8DFF");
  let changed = false;
  server.on("message", (m: any) => {
    if (m.command !== command || changed) return; changed = true;
    if (kind === "all-mics") for (const c of server.snapshot.state.channels) c.on = false;
    else if (kind === "plugin-group-all") for (const c of server.snapshot.state.channels) c.pluginGroups[0].off = true;
    else server.snapshot.state.channels[0].pluginGroups[0].off = true;
    server.snapshot.revision++; // External edit occurs at dispatch, before its delta is published.
  });
  host.keyDown("a");
  await until(server, "command", () => commands(server, command).length === 2);
  await key(host, "a", 1, kind === "all-mics" ? "#35D07F" : "#4C8DFF");
  const sent = commands(server, command);
  assert.deepEqual(sent.map(m => m.ifRevision), [1, 2]);
  assert.deepEqual(sent.map(m => kind === "all-mics" ? m.args.on : m.args.off), kind === "all-mics" ? [false, true] : [true, false]);
  assert.equal(alerts(host, "a").length, 0);
});

test("computed queue waits for ACK + state, keeps two newest intents, and alerts the displaced key", async t => {
  const { server, host, settings } = await setupActions(t);
  for (const [i, id] of ["active", "old", "new", "newest"].entries()) host.appear(id, settings, i, "all-mics");
  await key(host, "active", 0, "#FFB454"); server.behavior.omitDelta = true;
  host.keyDown("active"); await until(server, "command", () => commands(server).length === 1);
  host.keyDown("old"); host.keyDown("new"); host.keyDown("newest");
  await host.wait(() => alerts(host, "old").length === 1); assert.equal(commands(server).length, 1);
  server.behavior.omitDelta = false; server.publishDelta();
  await until(server, "command", () => commands(server).length === 3);
  assert.deepEqual(commands(server).map(m => m.args.on), [false, true, false]);
  assert.deepEqual(commands(server).map(m => m.ifRevision), [1, 2, 3]);
});

test("no microphones, offline and invalid settings never send commands; explicit failures alert once", async t => {
  const { server, host, settings } = await setupActions(t);
  server.mutate((s: Snapshot) => { s.state.channels = []; }, true);
  host.appear("empty", settings, 0, "all-mics");
  await host.wait(() => title(host, "empty") === "마이크 없음");
  host.keyDown("empty"); host.keyUp("empty"); await host.wait(() => alerts(host, "empty").length === 1);
  host.appear("bad", { ...settings, groupIndex: 6 }, 1, "plugin-group");
  await host.wait(() => title(host, "bad") === "동작 설정을 확인하세요.");
  host.keyDown("bad"); host.keyUp("bad"); await host.wait(() => alerts(host, "bad").length === 1);
  await server.setStatus("disabled"); host.appear("offline", settings, 2, "fx-mute-group");
  await host.wait(() => title(host, "offline") === "제어 꺼짐");
  host.keyDown("offline"); host.keyUp("offline"); await host.wait(() => alerts(host, "offline").length === 1);
  assert.equal(commands(server).length, 0);
});

test("each new action PI gets live channels/FX/slots/counts; offline options preserve saved selections", async t => {
  const { server, host, settings } = await setupActions(t, "en");
  for (const kind of ["all-mics", "mic-mute-group", "fx-mute-group", "plugin-group", "plugin-group-all", "fx-send", "fx-send-step", "status"]) {
    if (kind === "fx-send") host.appearDial(kind, settings); else host.appear(kind, { ...settings, ...(kind === "fx-send-step" ? { mode: "up" } : {}) }, 0, kind);
    const { pi, request } = await options(host, kind);
    await until(host, "pi", () => pi.messages.at(-1)?.payload.connection === "ready");
    const p = pi.messages.at(-1)!.payload;
    assert.deepEqual(p.channels[0].groupIndices, [1, 2]); assert.deepEqual(p.groupIndices, [1, 2]);
    assert.equal(p.fx.length, 2); assert.deepEqual(p.muteGroupCounts, { mic: 2, fx: 1 }); assert.equal(p.language, "en");
    assert.ok(!JSON.stringify(p).includes(server.token));
    request("new-request"); await until(host, "pi", () => pi.messages.at(-1)?.payload.requestId === "new-request");
    await host.closeInspector(kind);
  }
  const { pi, client } = await options(host, "fx-send"); await server.setStatus("disabled");
  await until(host, "pi", () => pi.messages.at(-1)?.payload.connection === "disabled");
  assert.equal(pi.messages.at(-1)!.payload.channels, undefined); assert.equal(pi.messages.at(-1)!.payload.fx, undefined);
  assert.equal(pi.messages.at(-1)!.payload.settings.fxId, settings.fxId);
  client.send(JSON.stringify({ event: "setSettings", context: pi.uuid, payload: { ...settings, stepPercent: 5, pressMode: "none" } }));
  await until(host, "pi", () => pi.messages.at(-1)?.payload.settings?.stepPercent === 5);
  await host.closeInspector("fx-send"); assert.equal(commands(server).length, 0);
});

test("pending computed intents expire after 500 ms without replay on later state", async t => {
  const { server, host, settings } = await setupActions(t);
  host.appear("a", settings, 0, "plugin-group"); host.appear("b", settings, 1, "plugin-group"); await key(host, "a", 1, "#4C8DFF");
  server.behavior.omitDelta = true; host.keyDown("a"); await until(server, "command", () => commands(server).length === 1);
  host.keyDown("b"); await host.wait(() => alerts(host, "b").length === 1);
  server.behavior.omitDelta = false; server.publishDelta(); await delay(80); await barrier(host, "b");
  assert.equal(commands(server).length, 1);
});

test("plugin-group-all: live PI options cover the union of slots and preserve the saved index offline", async t => {
  const { server, host } = await setupActions(t);
  server.mutate((s: Snapshot) => {
    s.state.channels[0]!.pluginGroups = [{ index: 1, off: false }];
    s.state.channels[1]!.pluginGroups.push({ index: 5, off: true });
  }, true);
  host.appear("a", { groupIndex: 5, mode: "toggle" }, 0, "plugin-group-all");
  await key(host, "a", 0, "#FF5A5F", "0/1");
  const { pi, client } = await options(host, "a");
  await until(host, "pi", () => pi.messages.at(-1)?.payload.connection === "ready");
  const p = pi.messages.at(-1)!.payload, counts = new Map<number, number>();
  for (const channel of p.channels) for (const index of channel.groupIndices) counts.set(index, (counts.get(index) ?? 0) + 1);
  assert.deepEqual([...counts].sort(([a], [b]) => a - b), [[1, 3], [2, 2], [5, 1]]);
  assert.equal(p.settings.groupIndex, 5); assert.equal(p.settings.channelId, "");
  await server.setStatus("disabled"); await until(host, "pi", () => pi.messages.at(-1)?.payload.connection === "disabled");
  assert.equal(pi.messages.at(-1)!.payload.channels, undefined); assert.equal(pi.messages.at(-1)!.payload.settings.groupIndex, 5);
  client.send(JSON.stringify({ event: "setSettings", context: pi.uuid, payload: { ...p.settings, mode: "off", shortTitle: "전체 효과" } }));
  await until(host, "pi", () => pi.messages.at(-1)?.payload.settings?.shortTitle === "전체 효과");
  assert.equal(pi.messages.at(-1)!.payload.settings.groupIndex, 5); assert.equal(commands(server).length, 0);
});
