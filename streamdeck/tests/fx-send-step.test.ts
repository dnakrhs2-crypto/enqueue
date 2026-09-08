import test, { type TestContext } from "node:test";
import assert from "node:assert/strict";
import { setTimeout as delay } from "node:timers/promises";
import type { Snapshot } from "../src/livemix/protocol.js";
import { until } from "./helpers.mjs";
import { setupActions, key, svg, title, commands, alerts, options, barrier } from "./action-helpers.js";

async function setup(t: TestContext, patch = {}, language = "ko") {
  const result = await setupActions(t, language);
  const settings = { ...result.settings, mode: "up", stepPercent: 5, targetPercent: 50, ...patch };
  result.host.appear("a", settings, 0, "fx-send-step");
  await key(result.host, "a", 0, "#35D07F", ">35%</text>");
  return { ...result, settings };
}
const send = (server: Awaited<ReturnType<typeof setup>>["server"]) => server.snapshot.state.channels[0].sends[0];
const closeTo = (actual: number, expected: number) => assert.ok(Math.abs(actual - expected) < 1e-10, `${actual} != ${expected}`);
function amountOnly(server: Awaited<ReturnType<typeof setup>>["server"]): void {
  for (const m of commands(server)) {
    assert.equal(m.command, "setSend"); assert.deepEqual(Object.keys(m.args).sort(), ["amount", "channelId", "fxId"]);
    assert.ok(Number.isInteger(m.ifRevision));
  }
}

for (const mode of ["up", "down"]) for (const stepPercent of [1, 5, 10]) test(`send key ${mode} ${stepPercent}%: amount-only CAS on down, inert release and live badge/title`, async t => {
  const { server, host, settings } = await setup(t, { mode, stepPercent });
  const expected = 35 + (mode === "up" ? stepPercent : -stepPercent);
  assert.equal(title(host, "a"), "마이크 1 → 리버브");
  assert.ok(svg(host.visual("a").image).includes(`>${mode === "up" ? "+" : "−"}${stepPercent}</text>`));
  host.keyDown("a");
  await key(host, "a", 0, "#35D07F", `>${expected}%</text>`);
  host.keyUp("a"); host.keyUp("a"); await barrier(host, "a");
  assert.equal(commands(server).length, 1); amountOnly(server); closeTo(send(server).amount, expected / 100);
  assert.equal(send(server).pre, false); assert.equal(commands(server)[0].ifRevision, 1);
  assert.equal(commands(server)[0].args.channelId, settings.channelId); assert.equal(commands(server)[0].args.fxId, settings.fxId);
});

test("send key defaults to up/5%, set defaults to 50%, and explicit targets 0/73/100 preserve pre", async t => {
  const { server, host, settings } = await setupActions(t);
  const selected = { channelId: settings.channelId, channelName: settings.channelName, fxId: settings.fxId, fxName: settings.fxName };
  host.appear("a", selected, 0, "fx-send-step"); await key(host, "a", 0, "#35D07F", ">+5</text>");
  host.keyDown("a"); await key(host, "a", 0, "#35D07F", ">40%</text>");
  assert.equal(host.contexts.get("a").settings.stepPercent, 5); assert.equal(host.contexts.get("a").settings.targetPercent, 50);
  server.mutate((s: Snapshot) => { s.state.channels[0]!.sends[0]!.pre = true; });
  for (const targetPercent of [undefined, 0, 73, 100]) {
    const expected = targetPercent ?? 50;
    host.settings("a", { ...selected, mode: "set", ...(targetPercent === undefined ? {} : { targetPercent }) });
    await host.wait(() => svg(host.visual("a").image).includes(`>=${expected}</text>`));
    host.keyDown("a"); host.keyUp("a");
    await key(host, "a", 0, expected ? "#35D07F" : "#3A3F47", `>${expected}%</text>`);
    assert.equal(send(server).amount, expected / 100); assert.equal(send(server).pre, true);
  }
  host.keyDown("a"); await until(server, "command", () => commands(server).length === 6);
  assert.equal(send(server).amount, 1); assert.equal(alerts(host, "a").length, 0); amountOnly(server);
});

for (const mode of ["up", "down"]) test(`send key ${mode} clamps at the endpoint and accepts a no-op ACK`, async t => {
  const { server, host } = await setup(t, { mode, stepPercent: 10 });
  server.mutate((s: Snapshot) => { s.state.channels[0]!.sends[0]!.amount = mode === "up" ? 0.98 : 0.02; s.state.channels[0]!.sends[0]!.pre = true; });
  await key(host, "a", 0, "#35D07F", `>${mode === "up" ? 98 : 2}%</text>`);
  host.keyDown("a"); const expected = mode === "up" ? 100 : 0;
  await key(host, "a", 0, expected ? "#35D07F" : "#3A3F47", `>${expected}%</text>`);
  host.keyDown("a"); await until(server, "command", () => commands(server).length === 2);
  assert.equal(send(server).amount, expected / 100); assert.equal(send(server).pre, true);
  assert.equal(alerts(host, "a").length, 0); amountOnly(server);
});

test("two send keys and a dial share the ACK/state barrier, fresh amounts and one LiveMix connection", async t => {
  const { server, host, settings } = await setup(t);
  host.appear("b", { ...settings, mode: "down", stepPercent: 1 }, 1, "fx-send-step");
  host.appearDial("dial", { ...settings, mode: "toggle", stepPercent: 1 });
  await key(host, "b", 0, "#35D07F", ">35%</text>"); await host.wait(() => host.feedback("dial").amount === "35%");
  server.behavior.omitDelta = true; host.keyDown("a"); await until(server, "command", () => commands(server).length === 1);
  host.keyDown("b"); host.dialRotate("dial", 2); await delay(80); await barrier(host, "b");
  assert.equal(commands(server).length, 1); assert.ok(svg(host.visual("a").image).includes(">35%</text>"));
  server.behavior.omitDelta = false; server.publishDelta();
  for (const id of ["a", "b"]) await key(host, id, 0, "#35D07F", ">41%</text>");
  await host.wait(() => host.feedback("dial").amount === "41%");
  assert.deepEqual(commands(server).map(m => m.ifRevision), [1, 2, 3]);
  commands(server).forEach((m, i) => closeTo(m.args.amount, [0.4, 0.39, 0.41][i]!));
  assert.equal(server.connections, 1); amountOnly(server);
});

for (const conflicts of [1, 2]) test(`send key recomputes at most once after a revision conflict (${conflicts} conflicts)`, async t => {
  const { server, host } = await setup(t);
  let count = 0;
  server.on("message", (m: any) => {
    if (m.command !== "setSend" || count++ >= conflicts) return;
    send(server).amount = count === 1 ? 0.8 : 0.6; send(server).pre = true; server.snapshot.revision++;
  });
  host.keyDown("a");
  await key(host, "a", 0, "#35D07F", `>${conflicts === 1 ? 85 : 60}%</text>`);
  if (conflicts === 2) await host.wait(() => alerts(host, "a").length === 1);
  await barrier(host, "a"); await delay(80);
  assert.equal(commands(server).length, 2); assert.deepEqual(commands(server).map(m => m.ifRevision), [1, 2]);
  closeTo(commands(server)[0].args.amount, 0.4); closeTo(commands(server)[1].args.amount, 0.85);
  assert.equal(send(server).pre, true); assert.equal(alerts(host, "a").length, conflicts === 1 ? 0 : 1); amountOnly(server);
});

test("send keys retain only two waiting intents and alert the displaced key", async t => {
  const { server, host, settings } = await setup(t);
  for (const [i, id] of ["old", "new", "newest"].entries()) host.appear(id, settings, i + 1, "fx-send-step");
  await key(host, "newest", 0, "#35D07F", ">35%</text>");
  server.behavior.omitDelta = true; host.keyDown("a"); await until(server, "command", () => commands(server).length === 1);
  host.keyDown("old"); host.keyDown("new"); host.keyDown("newest");
  await host.wait(() => alerts(host, "old").length === 1); assert.equal(commands(server).length, 1);
  server.behavior.omitDelta = false; server.publishDelta();
  await key(host, "newest", 0, "#35D07F", ">50%</text>");
  assert.equal(commands(server).length, 3); assert.deepEqual(commands(server).map(m => m.ifRevision), [1, 2, 3]);
});

test("send key waiting intent expires after 500 ms and is not replayed by later state", async t => {
  const { server, host, settings } = await setup(t);
  host.appear("b", settings, 1, "fx-send-step"); await key(host, "b", 0, "#35D07F", ">35%</text>");
  server.behavior.omitDelta = true; host.keyDown("a"); await until(server, "command", () => commands(server).length === 1);
  host.keyDown("b"); await host.wait(() => alerts(host, "b").length === 1);
  server.behavior.omitDelta = false; server.publishDelta();
  await key(host, "b", 0, "#35D07F", ">40%</text>"); await delay(80); await barrier(host, "b");
  assert.equal(commands(server).length, 1);
});

test("send key EOF clears the number to — and drops active/waiting input across reconnect", async t => {
  const { server, host } = await setup(t);
  server.behavior.holdCommands = true; host.keyDown("a"); await until(server, "message", () => commands(server).length === 1);
  host.keyDown("a"); await barrier(host, "a"); server.behavior.holdHello = true; server.eof();
  await host.wait(() => svg(host.visual("a").image).includes(">—</text>"));
  assert.ok(!/>\d+%<\//.test(svg(host.visual("a").image)));
  server.behavior.holdCommands = false; server.behavior.holdHello = false; await server.changePort();
  await key(host, "a", 0, "#35D07F", ">35%</text>"); await delay(80); assert.equal(commands(server).length, 1);
});

test("disabled send key shows —, keeps a status title, and alerts explicit input without sending", async t => {
  const { server, host, settings } = await setup(t);
  await server.setStatus("disabled"); await host.wait(() => title(host, "a") === "제어 꺼짐" && svg(host.visual("a").image).includes(">—</text>"));
  host.appear("fresh", settings, 1, "fx-send-step");
  await host.wait(() => title(host, "fresh") === "제어 꺼짐" && svg(host.visual("fresh").image).includes(">—</text>"));
  host.keyDown("fresh"); host.keyUp("fresh"); await host.wait(() => alerts(host, "fresh").length === 1);
  assert.equal(commands(server).length, 0);
  for (const r of host.records.filter(r => r.context === "fresh" && r.event === "setImage")) assert.ok(!/>\d+%<\//.test(svg(r.payload.image)));
});

for (const missing of ["channel", "fx"]) test(`send key missing ${missing} shows — and cannot command`, async t => {
  const { server, host } = await setup(t);
  server.mutate((s: Snapshot) => {
    if (missing === "channel") s.state.channels = [];
    else { s.state.fx = []; for (const channel of s.state.channels) channel.sends = []; }
  }, true);
  await host.wait(() => title(host, "a") === (missing === "channel" ? "채널 없음" : "FX 채널 없음"));
  assert.ok(svg(host.visual("a").image).includes("stroke-dasharray")); assert.ok(svg(host.visual("a").image).includes(">—</text>"));
  host.keyDown("a"); host.keyUp("a"); await host.wait(() => alerts(host, "a").length === 1); assert.equal(commands(server).length, 0);
});

test("send key creates an absent send from zero/post defaults", async t => {
  const { server, host } = await setup(t);
  server.mutate((s: Snapshot) => { s.state.channels[0]!.sends = []; }); await key(host, "a", 0, "#3A3F47", ">0%</text>");
  host.keyDown("a"); await key(host, "a", 0, "#35D07F", ">5%</text>");
  assert.equal(send(server).pre, false); amountOnly(server);
});

for (const language of ["ko", "en"]) test(`send key ${language} PI offers live channel/FX lists, selection and offline retention`, async t => {
  const { server, host, settings } = await setup(t, {}, language);
  const { pi, client } = await options(host, "a");
  await until(host, "pi", () => pi.messages.at(-1)?.payload.connection === "ready");
  const p = pi.messages.at(-1)!.payload;
  assert.equal(p.language, language); assert.equal(p.channels.length, 3); assert.equal(p.fx.length, 2);
  assert.equal(p.settings.mode, "up"); assert.equal(p.settings.stepPercent, 5); assert.equal(p.settings.targetPercent, 50);
  server.mutate((s: Snapshot) => { s.state.fx[1]!.name = "공간 <&>"; });
  await until(host, "pi", () => pi.messages.at(-1)?.payload.fx?.[1]?.name === "공간 <&>");
  const selected = { ...settings, channelId: p.channels[1].id, channelName: p.channels[1].name, fxId: p.fx[1].id, fxName: "공간 <&>" };
  client.send(JSON.stringify({ event: "setSettings", context: pi.uuid, payload: selected }));
  await host.wait(() => title(host, "a") === "게스트 → 공간 <&>" && svg(host.visual("a").image).includes(">0%</text>"));
  host.keyDown("a"); await key(host, "a", 0, "#35D07F", ">5%</text>");
  assert.equal(commands(server)[0].args.channelId, selected.channelId); assert.equal(commands(server)[0].args.fxId, selected.fxId);
  await server.setStatus("disabled"); await until(host, "pi", () => pi.messages.at(-1)?.payload.connection === "disabled");
  const offline = pi.messages.at(-1)!.payload;
  assert.equal(offline.channels, undefined); assert.equal(offline.fx, undefined); assert.equal(offline.settings.fxId, selected.fxId);
  assert.ok(!JSON.stringify(pi.messages).includes(server.token));
});

test("send key keeps mute/audio cues and a rolling 10-call budget through updates and reappearance", async t => {
  const { server, host, settings } = await setup(t, {}, "en");
  server.mutate((s: Snapshot) => { s.state.muteGroups.fx = true; s.state.audio.running = false; });
  await key(host, "a", 0, "#FF5A5F", "Mute group"); assert.ok(svg(host.visual("a").image).includes("Audio stopped"));
  for (let n = 1; n <= 35; n++) { server.mutate((s: Snapshot) => { s.state.channels[0]!.sends[0]!.amount = n / 100; }); await delay(8); }
  await host.disappear("a"); host.appear("a", settings, 0, "fx-send-step");
  server.mutate((s: Snapshot) => { s.state.channels[0]!.sends[0]!.amount = 0.9; });
  await key(host, "a", 0, "#FF5A5F", ">90%</text>"); host.assertBudget();
  assert.ok(host.records.filter(r => r.event === "setState").every(r => r.payload.state === 0));
});
