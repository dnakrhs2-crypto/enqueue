import test, { type TestContext } from "node:test";
import assert from "node:assert/strict";
import { setTimeout as delay } from "node:timers/promises";
import { DeviceType } from "@elgato/streamdeck";
import type { Snapshot } from "../src/livemix/protocol.js";
import { until } from "./helpers.mjs";
import { setupActions, commands, alerts, barrier, options } from "./action-helpers.js";

async function setup(t: TestContext, language = "ko") {
  const result = await setupActions(t, language);
  result.host.appearDial("a", result.settings);
  await result.host.wait(() => result.host.feedback("a").amount === "35%");
  assert.equal(result.host.info.devices.find(d => d.id === "fake-plus")!.type, DeviceType.StreamDeckPlus);
  return result;
}
const send = (server: Awaited<ReturnType<typeof setup>>["server"]) => server.snapshot.state.channels[0].sends[0];
const closeTo = (actual: number, expected: number) => assert.ok(Math.abs(actual - expected) < 1e-10, `${actual} != ${expected}`);

test("dial ±ticks and 1%/5% steps update canonical amount with amount-only CAS and live feedback", async t => {
  const { server, host, settings } = await setup(t);
  assert.deepEqual(host.feedback("a"), { name: "마이크 1 → 리버브", amount: "35%", mode: "포스트", level: 35, status: "" });
  host.dialRotate("a", 3); await host.wait(() => host.feedback("a").amount === "38%");
  host.dialRotate("a", -8); await host.wait(() => host.feedback("a").amount === "30%");
  host.settings("a", { ...settings, stepPercent: 5 }); host.dialRotate("a", 2);
  await host.wait(() => host.feedback("a").amount === "40%"); closeTo(send(server).amount, 0.4);
  assert.equal(send(server).pre, false); assert.equal(commands(server).length, 3);
  for (const m of commands(server)) { assert.deepEqual(Object.keys(m.args).sort(), ["amount", "channelId", "fxId"]); assert.ok(Number.isInteger(m.ifRevision)); }
});

test("dial saturates at 0/100%, preserves pre, and accepts no-op acknowledgements", async t => {
  const { server, host } = await setup(t);
  server.mutate((s: Snapshot) => { s.state.channels[0]!.sends[0]!.pre = true; });
  await host.wait(() => host.feedback("a").mode === "프리");
  for (const [ticks, amount] of [[50, "85%"], [50, "100%"]] as const) { host.dialRotate("a", ticks); await host.wait(() => host.feedback("a").amount === amount); }
  host.dialRotate("a", 1); await until(server, "command", () => commands(server).length === 3);
  assert.equal(send(server).amount, 1);
  for (const amount of ["50%", "0%"]) { host.dialRotate("a", -50); await host.wait(() => host.feedback("a").amount === amount); }
  host.dialRotate("a", -1); await until(server, "command", () => commands(server).length === 6);
  assert.equal(send(server).amount, 0); assert.equal(send(server).pre, true);
  assert.equal(alerts(host, "a").length, 0);
});

test("absent send starts at LiveMix's 0/post defaults and is created by amount-only input", async t => {
  const { server, host } = await setup(t);
  server.mutate((s: Snapshot) => { s.state.channels[0]!.sends = []; });
  await host.wait(() => host.feedback("a").amount === "0%"); host.dialRotate("a", 1);
  await host.wait(() => host.feedback("a").amount === "1%"); assert.equal(send(server).pre, false);
});

test("channel/FX renames persist together; both bindings resolve a new session without overwriting original IDs", async t => {
  const { server, host, settings } = await setup(t);
  server.mutate((s: Snapshot) => { s.state.channels[0]!.name = "진행"; s.state.fx[0]!.name = "공간"; });
  await host.wait(() => host.feedback("a").name === "진행 → 공간" && host.contexts.get("a").settings.channelName === "진행" && host.contexts.get("a").settings.fxName === "공간");
  const channelId = "66666666666646668666666666666666", fxId = "77777777777747778777777777777777";
  server.mutate((s: Snapshot) => {
    s.sessionId = "cccccccccccc4ccc8ccccccccccccccc"; s.state.channels[0]!.id = channelId; s.state.fx[0]!.id = fxId;
    for (const c of s.state.channels) for (const send of c.sends) if (send.fxId === settings.fxId) send.fxId = fxId;
  }, true);
  const inspector = await options(host, "a");
  await until(host, "pi", () => inspector.pi.messages.at(-1)?.payload.sessionId === server.snapshot.sessionId);
  host.dialRotate("a", 1); await host.wait(() => host.feedback("a").amount === "36%");
  assert.equal(commands(server)[0].args.channelId, channelId); assert.equal(commands(server)[0].args.fxId, fxId);
  assert.equal(host.contexts.get("a").settings.channelId, settings.channelId); assert.equal(host.contexts.get("a").settings.fxId, settings.fxId);
});

test("ticks within 50 ms coalesce across two encoders on one target, including different steps", async t => {
  const { server, host, settings } = await setup(t);
  host.appearDial("b", { ...settings, stepPercent: 5 }, 1); await host.wait(() => host.feedback("b").amount === "35%");
  host.dialRotate("a", 2); await delay(15); host.dialRotate("b", 1);
  await host.wait(() => host.feedback("a").amount === "42%" && host.feedback("b").amount === "42%");
  assert.equal(commands(server).length, 1); closeTo(send(server).amount, 0.42); assert.equal(server.connections, 1);
});

test("rotation and press wait for ACK and canonical state, preserving input order on one target", async t => {
  const { server, host } = await setup(t);
  server.behavior.omitDelta = true; host.dialRotate("a", 1);
  await until(server, "command", () => commands(server).length === 1);
  host.dialRotate("a", 2); host.dialDown("a"); host.dialUp("a");
  await delay(90); assert.equal(commands(server).length, 1, "An ACK alone cannot release the next rotation");
  assert.equal(host.feedback("a").amount, "35%");
  server.behavior.omitDelta = false; server.publishDelta();
  await host.wait(() => host.feedback("a").amount === "38%" && host.feedback("a").mode === "프리");
  assert.deepEqual(commands(server).map(m => Object.keys(m.args).sort()), [["amount", "channelId", "fxId"], ["amount", "channelId", "fxId"], ["channelId", "fxId", "pre"]]);
  closeTo(send(server).amount, 0.38);
});

test("canonical state without ACK cannot release a second send", async t => {
  const { server, host } = await setup(t);
  server.behavior.omitAck = true; host.dialRotate("a", 1); await host.wait(() => host.feedback("a").amount === "36%");
  const first = commands(server)[0]; host.dialRotate("a", 2); await delay(90);
  assert.equal(commands(server).length, 1);
  server.behavior.omitAck = false;
  server.send([...server.clients][0], { v: 1, type: "ack", id: first.id, ...server.context(), changed: true, result: { amount: 0.36, pre: false } });
  await host.wait(() => host.feedback("a").amount === "38%"); assert.equal(commands(server).length, 2);
});

test("more than 50 absolute unsent ticks drops the accumulation and shows Input delayed once", async t => {
  const { server, host } = await setup(t);
  host.dialRotate("a", 30); host.dialRotate("a", -21);
  await host.wait(() => host.feedback("a").status === "입력 지연" && alerts(host, "a").length === 1);
  await delay(100); assert.equal(commands(server).length, 0); closeTo(send(server).amount, 0.35);
  host.dialRotate("a", 50); await host.wait(() => host.feedback("a").amount === "85%" && host.feedback("a").status === "");
  assert.equal(commands(server).length, 1);
});

test("an oversized single event is rejected and unsent accumulation expires after 500 ms", async t => {
  const { server, host } = await setup(t);
  host.dialRotate("a", 51); await host.wait(() => alerts(host, "a").length === 1);
  assert.equal(commands(server).length, 0);
  server.behavior.omitDelta = true; host.dialRotate("a", 1); await until(server, "command", () => commands(server).length === 1);
  host.dialRotate("a", 10); await host.wait(() => alerts(host, "a").length === 2);
  server.behavior.omitDelta = false; server.publishDelta();
  await host.wait(() => host.feedback("a").amount === "36%"); await delay(80);
  assert.equal(commands(server).length, 1); assert.equal(host.feedback("a").status, "입력 지연");
});

test("server-side amount/pre edits cause conflict and recomputation from the new state", async t => {
  const { server, host } = await setup(t);
  let changed = false;
  server.on("message", (m: any) => {
    if (m.command !== "setSend" || changed) return; changed = true;
    send(server).amount = 0.8; send(server).pre = true; server.snapshot.revision++;
  });
  host.dialRotate("a", 5);
  await host.wait(() => host.feedback("a").amount === "85%" && host.feedback("a").mode === "프리");
  const sent = commands(server); assert.equal(sent.length, 2); closeTo(sent[0].args.amount, 0.4); closeTo(sent[1].args.amount, 0.85);
  assert.deepEqual(sent.map(m => m.ifRevision), [1, 2]); assert.ok(sent.every(m => !Object.hasOwn(m.args, "pre"))); assert.equal(alerts(host, "a").length, 0);
});

for (const conflicts of [2, 3]) test(`dial conflict retries are bounded to two (${conflicts} external conflicts)`, async t => {
  const { server, host } = await setup(t);
  let count = 0;
  server.on("message", (m: any) => {
    if (m.command !== "setSend" || count++ >= conflicts) return;
    send(server).amount += 0.1; server.snapshot.revision++;
  });
  host.dialRotate("a", 5);
  if (conflicts === 2) { await host.wait(() => host.feedback("a").amount === "60%"); assert.equal(alerts(host, "a").length, 0); }
  else { await host.wait(() => alerts(host, "a").length === 1 && host.feedback("a").amount === "65%"); }
  assert.equal(commands(server).length, 3); await delay(100); assert.equal(commands(server).length, 3);
});

test("short press records on down and toggles only pre on release, preserving amount", async t => {
  const { server, host } = await setup(t);
  host.dialDown("a"); await barrier(host, "a"); assert.equal(commands(server).length, 0);
  server.mutate((s: Snapshot) => { s.state.channels[0]!.sends[0]!.amount = 0.72; });
  await host.wait(() => host.feedback("a").amount === "72%"); host.dialUp("a");
  await host.wait(() => host.feedback("a").mode === "프리");
  assert.equal(send(server).amount, 0.72); assert.deepEqual(commands(server)[0].args, { channelId: server.snapshot.state.channels[0].id, fxId: server.snapshot.state.fx[0].id, pre: true });
  host.dialUp("a"); await barrier(host, "a"); assert.equal(commands(server).length, 1, "Duplicate release is inert");
});

test("pre/post conflicts refresh the state and alert once without retrying a toggle", async t => {
  const { server, host } = await setup(t);
  server.once("command", () => assert.fail("The stale press must be rejected before application"));
  server.on("message", (m: any) => {
    if (m.command !== "setSend") return;
    send(server).pre = true; send(server).amount = 0.7; server.snapshot.revision++;
  });
  host.dialDown("a"); host.dialUp("a");
  await host.wait(() => alerts(host, "a").length === 1 && host.feedback("a").amount === "70%" && host.feedback("a").mode === "프리");
  await delay(80); assert.equal(commands(server).length, 1); assert.equal(Object.hasOwn(commands(server)[0].args, "amount"), false);
});

test("press + rotation changes amount only; release does not toggle pre/post", async t => {
  const { server, host } = await setup(t);
  host.dialDown("a"); host.dialRotate("a", 4, true); host.dialUp("a");
  await host.wait(() => host.feedback("a").amount === "39%"); await barrier(host, "a");
  assert.equal(commands(server).length, 1); assert.equal(send(server).pre, false); assert.equal(Object.hasOwn(commands(server)[0].args, "pre"), false);
});

test("long press, disabled press mode, tap and long touch are inert; disabled press still permits rotation", async t => {
  const { server, host, settings } = await setup(t);
  host.dialDown("a"); await delay(650); host.dialUp("a"); host.touch("a"); host.touch("a", true); await barrier(host, "a");
  assert.equal(commands(server).length, 0);
  host.settings("a", { ...settings, pressMode: "none" }); host.dialDown("a"); host.dialUp("a"); await barrier(host, "a");
  assert.equal(commands(server).length, 0); host.dialRotate("a", 1); await host.wait(() => host.feedback("a").amount === "36%");
  assert.equal(send(server).pre, false);
});

test("session change drops pending accumulation and a held press; fresh input resolves the new session", async t => {
  const { server, host } = await setup(t);
  server.behavior.omitAck = true; host.dialRotate("a", 1); await host.wait(() => host.feedback("a").amount === "36%");
  host.dialRotate("a", 10); host.dialDown("a"); await barrier(host, "a");
  server.behavior.omitAck = false;
  server.mutate((s: Snapshot) => { s.sessionId = "cccccccccccc4ccc8ccccccccccccccc"; s.state.channels[0]!.sends[0]!.amount = 0.2; }, true);
  await host.wait(() => host.feedback("a").amount === "20%"); host.dialUp("a"); await delay(100);
  assert.equal(commands(server).length, 1); assert.equal(send(server).pre, false);
  host.dialRotate("a", 1); await host.wait(() => host.feedback("a").amount === "21%");
  assert.equal(commands(server).at(-1).sessionId, server.snapshot.sessionId);
});

for (const failure of ["EOF", "timeout"]) test(`${failure} drops remaining ticks; offline shows an em dash, never zero percent`, async t => {
  const { server, host } = await setup(t);
  server.behavior.holdCommands = true; host.dialRotate("a", 1);
  await until(server, "message", () => commands(server).length === 1); host.dialRotate("a", 10); await barrier(host, "a");
  server.behavior.holdHello = true;
  if (failure === "EOF") server.eof();
  await host.wait(() => host.feedback("a").amount === "—", 4000);
  assert.equal(host.contexts.get("a").feedback.level.enabled, false); assert.ok(host.feedback("a").status);
  server.behavior.holdCommands = false; server.behavior.holdHello = false; await server.changePort();
  await host.wait(() => host.feedback("a").amount === "35%"); await delay(100); assert.equal(commands(server).length, 1);
});

test("offline rotation and short press each alert once; missing FX is persistent and cannot command", async t => {
  const { server, host } = await setup(t);
  server.mutate((s: Snapshot) => { s.state.fx = []; for (const c of s.state.channels) c.sends = []; }, true);
  await host.wait(() => host.feedback("a").amount === "—" && host.feedback("a").status === "FX 채널 없음");
  host.dialRotate("a", 1); await host.wait(() => alerts(host, "a").length === 1);
  await server.setStatus("disabled"); await host.wait(() => host.feedback("a").status === "제어 꺼짐");
  host.dialDown("a"); host.dialUp("a"); await host.wait(() => alerts(host, "a").length === 2);
  assert.equal(host.feedback("a").amount, "—"); assert.equal(commands(server).length, 0);
});

test("disappearance cancels that encoder's waiting ticks while another encoder's contribution survives", async t => {
  const { server, host, settings } = await setup(t);
  host.appearDial("b", settings, 1); await host.wait(() => host.feedback("b").amount === "35%");
  server.behavior.omitDelta = true; host.dialRotate("b", 1); await until(server, "command", () => commands(server).length === 1);
  host.dialRotate("a", 5); host.dialRotate("b", 2); host.disappear("a"); await barrier(host, "b");
  server.behavior.omitDelta = false; server.publishDelta();
  await host.wait(() => host.feedback("b").amount === "38%"); assert.equal(commands(server).length, 2);
});

test("feedback follows mute groups/audio, localizes in English and stays within 10 calls per rolling second", async t => {
  const { server, host, settings } = await setup(t, "en");
  assert.equal(host.feedback("a").mode, "Post");
  server.mutate((s: Snapshot) => { s.state.muteGroups.fx = true; }); await host.wait(() => host.feedback("a").status === "Mute group applied");
  server.mutate((s: Snapshot) => { s.state.muteGroups.fx = false; s.state.audio.running = false; }); await host.wait(() => host.feedback("a").status === "Audio stopped");
  for (let n = 1; n <= 35; n++) {
    server.mutate((s: Snapshot) => { s.state.channels[0]!.sends[0]!.amount = n / 100; }); await delay(8);
  }
  host.disappear("a"); host.appearDial("a", settings);
  server.mutate((s: Snapshot) => { s.state.channels[0]!.sends[0]!.amount = 0.9; });
  await host.wait(() => host.feedback("a").amount === "90%"); host.assertBudget();
  const inspector = await options(host, "a"); assert.equal(inspector.pi.messages.at(-1)!.payload.language, "en");
});
