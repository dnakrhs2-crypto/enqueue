import test, { type TestContext } from "node:test";
import assert from "node:assert/strict";
import { FakeHost } from "./fake-host.mjs";
import { FakeLiveMix } from "./fake-livemix-server.mjs";
import { until } from "./helpers.mjs";
import { migrateSettings } from "../src/livemix/bindings.js";
import { readDiscovery } from "../src/livemix/connection.js";

const svg = (url: string | undefined): string => url?.startsWith("data:image/svg+xml;base64,") ? Buffer.from(url.split(",")[1]!, "base64").toString("utf8") : "";
// §4.4 allows status titles to wrap at word boundaries.
const titles = (s: string | undefined): string => s?.replace(/\n/g, " ") ?? "";
async function setup(t: TestContext, language = "ko", status = "ready") {
  const server = await new FakeLiveMix().start();
  if (status !== "ready") await server.setStatus(status);
  const host = new FakeHost();
  t.after(async () => { try { await host.close(); } finally { await server.close(); } });
  await host.start(server.appdata, language);
  const settings = migrateSettings({ channelId: server.snapshot.state.channels[0].id, channelName: server.snapshot.state.channels[0].name }).settings;
  return { server, host, settings };
}
async function lamp(host: FakeHost, context: string, state: number, color: string, label = "") {
  // §4.4 sends state → image → title. Muted ON/OFF share a color, so wait for the label too.
  await host.wait(() => {
    const visual = host.visual(context), image = svg(visual.image);
    return visual.state === state && image.includes(color) && image.includes(label);
  });
}
test("real SDK registerPlugin, keyDown → TCP command → new key state/image; two keys share one connection", async t => {
  const { server, host, settings } = await setup(t);
  host.appear("key-a", settings); host.appear("key-b", settings, 1);
  await lamp(host, "key-a", 1, "#35D07F"); await lamp(host, "key-b", 1, "#35D07F");
  const before = host.visual("key-a").image;
  host.keyDown("key-a"); host.keyUp("key-a");
  await lamp(host, "key-a", 0, "#3A3F47"); await lamp(host, "key-b", 0, "#3A3F47");
  assert.equal(server.snapshot.state.channels[0].on, false); assert.notEqual(host.visual("key-a").image, before);
  assert.equal(server.received.filter((m: { command?: string }) => m.command === "toggleChannel").length, 1);
  assert.equal(server.connections, 1); assert.deepEqual(host.registration, { event: "registerPlugin", uuid: "test-runtime-1" });
  assert.ok(!host.output.includes(server.token)); host.assertBudget();
});
test("willAppear draws cached state immediately; settings select explicit ON/OFF and keyUp is inert", async t => {
  const { server, host, settings } = await setup(t); host.appear("key-a", settings); await lamp(host, "key-a", 1, "#35D07F");
  host.appear("key-b", { ...settings, mode: "off" }, 1); await lamp(host, "key-b", 1, "#35D07F");
  host.keyDown("key-b"); host.keyUp("key-b"); await lamp(host, "key-b", 0, "#3A3F47");
  host.settings("key-b", { ...settings, mode: "on" }); host.keyDown("key-b"); host.keyUp("key-b"); await lamp(host, "key-b", 1, "#35D07F");
  assert.deepEqual(server.received.filter((m: { command?: string }) => m.command === "setChannelOn").map((m: { args: { on: boolean } }) => m.args.on), [false, true]);
  assert.equal(server.connections, 1);
});
test("stopped server removes green lamps, shows offline and alerts once on explicit input", async t => {
  const { server, host, settings } = await setup(t);
  host.appear("key-a", settings); host.appear("key-b", settings, 1);
  await lamp(host, "key-a", 1, "#35D07F"); await lamp(host, "key-b", 1, "#35D07F");
  await server.setStatus("stopped");
  await host.wait(() => ["key-a", "key-b"].every(context => titles(host.visual(context).title) === "LiveMix 미연결"));
  for (const context of ["key-a", "key-b"]) {
    assert.equal(host.visual(context).state, 0);
    assert.ok(!svg(host.visual(context).image).includes("#35D07F"));
  }
  assert.equal(host.records.filter(r => r.event === "showAlert").length, 0);
  host.keyDown("key-a"); host.keyUp("key-a"); await host.wait(() => host.records.some(r => r.event === "showAlert"));
  assert.deepEqual(host.records.filter(r => r.event === "showAlert").map(r => r.context), ["key-a"]);
  assert.equal(server.received.filter((m: { type: string }) => m.type === "command").length, 0);
});

test("bare TCP EOF clears every visible key while discovery still says ready", async t => {
  const { server, host, settings } = await setup(t);
  host.appear("key-a", settings); host.appear("key-b", settings, 1);
  await lamp(host, "key-a", 1, "#35D07F"); await lamp(host, "key-b", 1, "#35D07F");
  // No serverStatus or discovery update: only the socket's end/close events can invalidate readiness.
  server.behavior.holdHello = true; server.eof();
  // A retry can begin before the rolling render budget renews; §4.4 also permits checking then.
  await host.wait(() => ["key-a", "key-b"].every(context => {
    const visual = host.visual(context);
    return visual.state === 0 && !svg(visual.image).includes("#35D07F")
      && ["LiveMix 미연결", "연결 확인 중"].includes(titles(visual.title));
  }));
  for (const context of ["key-a", "key-b"]) {
    assert.equal(host.visual(context).state, 0);
    assert.ok(!svg(host.visual(context).image).includes("#35D07F"));
  }
  assert.equal((await readDiscovery(server.discoveryPath)).state, "ready");
  assert.equal(host.records.filter(r => r.event === "showAlert").length, 0);
  host.keyDown("key-b"); host.keyUp("key-b");
  await host.wait(() => host.records.some(r => r.event === "showAlert"));
  assert.deepEqual(host.records.filter(r => r.event === "showAlert").map(r => r.context), ["key-b"]);
  assert.equal(server.received.filter((m: { type: string }) => m.type === "command").length, 0);
});
test("fresh disabled discovery and missing binding render persistent statuses", async t => {
  const { server, host, settings } = await setup(t, "ko", "disabled"); host.appear("key-a", settings);
  await host.wait(() => host.visual("key-a").title === "제어 꺼짐"); assert.equal(server.connections, 0);
  server.discoveryState = "ready"; await server.publishDiscovery(); await lamp(host, "key-a", 1, "#35D07F");
  server.mutate((s: { state: { channels: unknown[] } }) => { s.state.channels = []; }, true);
  await host.wait(() => host.visual("key-a").title === "채널 없음");
  assert.ok(svg(host.visual("key-a").image).includes("stroke-dasharray"));
  host.keyDown("key-a"); await host.wait(() => host.records.some(r => r.event === "showAlert"));
});
test("mute-group badge is red but state remains the original ON/OFF; audio stopped stays controllable", async t => {
  const { server, host, settings } = await setup(t); host.appear("key-a", settings); await lamp(host, "key-a", 1, "#35D07F");
  server.mutate((s: { state: { muteGroups: { mic: boolean }; channels: { muteGroup: boolean }[] } }) => { s.state.muteGroups.mic = true; s.state.channels[0]!.muteGroup = true; });
  await lamp(host, "key-a", 1, "#FF5A5F", "원래 ON"); assert.ok(svg(host.visual("key-a").image).includes("원래 ON"));
  host.keyDown("key-a"); await lamp(host, "key-a", 0, "#FF5A5F", "원래 OFF");
  assert.ok(svg(host.visual("key-a").image).includes("원래 OFF")); assert.ok(svg(host.visual("key-a").image).includes("오디오 멈춤"));
  assert.equal(server.snapshot.state.channels[0].on, false);
});
test("rapid external updates coalesce within 10 total programmatic calls/key/rolling second", async t => {
  const { server, host, settings } = await setup(t); host.appear("key-a", settings); await lamp(host, "key-a", 1, "#35D07F");
  for (let i = 0; i < 31; i++) server.mutate((s: { state: { channels: { on: boolean }[] } }) => { s.state.channels[0]!.on = !s.state.channels[0]!.on; });
  await lamp(host, "key-a", 0, "#3A3F47"); host.assertBudget();
  const count = host.records.length; server.broadcastState();
  const inspector = await host.openInspector("key-a"); inspector.request();
  await until(host, "pi", () => inspector.pi.messages.some((m: { payload: { op: string } }) => m.payload.op === "options"));
  assert.equal(host.records.length, count, "Identical state causes no redundant rendering");
});
test("PI has a real separate registration/relay, offline selection persists and names stay data", async t => {
  const { server, host, settings } = await setup(t, "en"); const name = '<img src=x onerror="alert(1)"> & 곰';
  server.mutate((s: { state: { channels: { name: string }[] } }) => { s.state.channels[0]!.name = name; });
  host.appear("key-a", settings); await lamp(host, "key-a", 1, "#35D07F");
  const { pi, client, request } = await host.openInspector("key-a"); request("pi-safe");
  await until(host, "pi", () => pi.messages.some((m: { payload: { connection: string } }) => m.payload.connection === "ready"));
  const options = pi.messages.at(-1)!.payload;
  assert.equal(options.channels[0].name, name); assert.equal(options.settings.channelId, settings.channelId); assert.equal(options.language, "en");
  assert.equal(JSON.stringify(options).includes(server.token), false);
  assert.ok(!svg(host.visual("key-a").image).includes("onerror"));
  const renamed = name + " 2";
  server.mutate((s: { state: { channels: { name: string }[] } }) => { s.state.channels[0]!.name = renamed; });
  await until(host, "pi", () => pi.messages.some(m => m.event === "didReceiveSettings" && m.payload.settings.channelName === renamed));
  const echo = pi.messages.find(m => m.event === "didReceiveSettings" && m.payload.settings.channelName === renamed)!;
  assert.equal(echo.context, pi.uuid); assert.equal(echo.payload.settings.channelId, settings.channelId);
  await server.setStatus("disabled");
  await until(host, "pi", () => pi.messages.at(-1)?.payload.connection === "disabled");
  assert.equal(pi.messages.at(-1)!.payload.channels, undefined); assert.equal(pi.messages.at(-1)!.payload.settings.channelId, settings.channelId);
  await host.wait(() => titles(host.visual("key-a").title) === "Control disabled");
  const edited = { ...echo.payload.settings, mode: "off", shortTitle: "Offline edit" };
  client.send(JSON.stringify({ event: "setSettings", context: pi.uuid, payload: edited }));
  await until(host, "pi", () => pi.messages.some(m => m.event === "sendToPropertyInspector" && m.payload.connection === "disabled"
    && m.payload.settings.mode === "off" && m.payload.settings.shortTitle === edited.shortTitle));
  assert.deepEqual(host.contexts.get("key-a")!.settings, edited);
  assert.deepEqual(pi.messages.at(-1)!.payload.settings, edited);
  assert.equal(pi.messages.at(-1)!.payload.channels, undefined);
  assert.equal(server.received.filter((m: { type: string }) => m.type === "command").length, 0);
  await host.closeInspector("key-a");
});
test("hidden keys retain deletion boundaries when their context reappears", async t => {
  const { server, host, settings } = await setup(t); host.appear("key-a", settings); await lamp(host, "key-a", 1, "#35D07F");
  host.disappear("key-a");
  server.mutate((s: { state: { channels: { id: string }[] } }) => { s.state.channels[0]!.id = "cccccccccccc4ccc8ccccccccccccccc"; }, true);
  host.appear("key-a", settings);
  await host.wait(() => host.visual("key-a").title === "채널 없음");
  host.keyDown("key-a"); await host.wait(() => host.records.some(r => r.event === "showAlert"));
  assert.equal(server.received.filter((m: { type: string }) => m.type === "command").length, 0);
});
