import test, { type TestContext } from "node:test";
import assert from "node:assert/strict";
import { LiveMixConnection, type ConnectionOptions } from "../src/livemix/connection.js";
import { CommandQueue } from "../src/livemix/command-queue.js";
import type { Snapshot } from "../src/livemix/protocol.js";
import { windowImage } from "../src/ui/key-renderer.js";
import { FakeLiveMix } from "./fake-livemix-server.mjs";
import type { FakeHost } from "./fake-host.mjs";
import { nextEvent, until } from "./helpers.mjs";
import { alerts, commands, key, options, setupActions } from "./action-helpers.js";

const openGroup = (index = 1) => ({ command: "setPluginGroupWindows" as const, args: { index, open: true } });
const micOn = (channelId: string) => ({ command: "setChannelOn" as const, args: { channelId, on: true } });
const readyWindow = (host: FakeHost, id: string, mode: "toggle" | "open" = "open") =>
  host.wait(() => host.visual(id).image === windowImage(mode, true, 1, "ko"));
function noFailures(server: FakeLiveMix, host: FakeHost, ids: string[]): void {
  assert.equal(server.connections, 1, "The original connection stayed open");
  assert.deepEqual(server.sent.filter(m => m.type === "error"), []);
  for (const id of ids) assert.equal(alerts(host, id).length, 0, id);
}
async function setupConnection(t: TestContext, timing: ConnectionOptions["timing"] = {}, windowDelayMs = 600) {
  const server = await new FakeLiveMix().start();
  server.snapshot.state.channels[0].pluginGroups = [{ index: 1, off: false }, { index: 2, off: false }];
  server.behavior.windowDelayMs = windowDelayMs;
  const connection = new LiveMixConnection({ discoveryPath: server.discoveryPath, timing, random: () => 0.5 });
  const queue = new CommandQueue(connection), disconnects: string[] = [];
  connection.on("disconnect", (code: string) => disconnects.push(code));
  t.after(async () => { queue.dispose(); connection.stop(); await server.close(); });
  connection.start(); await until(connection, "status", () => connection.ready);
  return { server, connection, queue, disconnects, channelId: server.snapshot.state.channels[0].id as string };
}

test("window wait: a real SDK key succeeds after an editor takes more than the normal two-second deadline", async t => {
  const { server, host } = await setupActions(t); server.behavior.windowDelayMs = 2300;
  host.appear("window", { mode: "open" }, 0, "plugin-window"); await readyWindow(host, "window");
  host.keyDown("window"); host.keyUp("window");
  await until(server, "command", () => commands(server).length === 1 && server.sent.some(m => m.type === "ack" && m.result.open === true));
  await options(host, "window"); await host.fence();
  noFailures(server, host, ["window"]);
  assert.equal(host.visual("window").state, 0); assert.equal(host.visual("window").image, windowImage("open", true, 1, "ko"));
  assert.equal(commands(server, "requestState").length, 0);
});

test("window wait: mic ON and status keys are sent in order after the editor ACK, without server expiry", async t => {
  const { server, host, settings } = await setupActions(t); server.behavior.windowDelayMs = 1200;
  server.mutate((s: Snapshot) => { s.state.channels[0]!.on = false; });
  host.appear("window", { mode: "open" }, 0, "plugin-window");
  host.appear("mic", { ...settings, mode: "on" }, 1); host.appear("status", {}, 2, "status");
  await readyWindow(host, "window"); await key(host, "mic", 0, "#3A3F47"); await key(host, "status", 1, "#35D07F");
  let receivedAtEnd: string[] = [];
  server.once("windowEnd", () => { receivedAtEnd = server.received.filter(m => m.type === "command").map(m => m.command); });
  host.keyDown("window"); await nextEvent(server, "windowStart");
  host.keyDown("mic"); host.keyUp("mic"); host.keyDown("status"); host.keyUp("status"); await host.fence();
  await key(host, "mic", 1, "#35D07F");
  await until(server, "message", () => commands(server, "requestState").length === 1); await host.fence();
  assert.deepEqual(receivedAtEnd, ["setPluginGroupWindows"]);
  assert.deepEqual(server.received.filter(m => m.type === "command").map(m => m.command), ["setPluginGroupWindows", "setChannelOn", "requestState"]);
  assert.deepEqual(commands(server, "setChannelOn")[0].args, { channelId: settings.channelId, on: true });
  noFailures(server, host, ["window", "mic", "status"]);
});

test("window wait: multi-action group 1 open then group 2 open completes both steps on one connection", async t => {
  const { server, host } = await setupActions(t); server.behavior.windowDelayMs = 1200;
  for (const index of [1, 2]) {
    const id = String(index); host.appear(id, { mode: "open", groupIndex: index }, 0, "plugin-window", "Keypad", { isInMultiAction: true });
    const { pi } = await options(host, id); await until(host, "pi", () => pi.messages.at(-1)?.payload.connection === "ready");
    await host.closeInspector(id);
  }
  let receivedAtFirstEnd = 0;
  server.once("windowEnd", () => { receivedAtFirstEnd = commands(server).length; });
  host.keyDown("1"); host.keyUp("1"); host.keyDown("2"); host.keyUp("2");
  await until(server, "command", () => server.sent.filter(m => m.type === "ack" && m.result.open === true).length === 2);
  await host.fence();
  assert.equal(receivedAtFirstEnd, 1);
  assert.deepEqual(commands(server).map(m => ({ command: m.command, args: m.args })), [openGroup(1), openGroup(2)]);
  assert.deepEqual(host.records, []); noFailures(server, host, ["1", "2"]);
});

test("window wait: multi-action mute then open waits for the delayed delta without STATE_TIMEOUT", async t => {
  const { server, host, settings } = await setupActions(t); server.behavior.windowDelayMs = 2300;
  host.appear("mute", { mode: "mute" }, 0, "mic-mute-all", "Keypad", { isInMultiAction: true });
  host.appear("window", { mode: "open" }, 0, "plugin-window", "Keypad", { isInMultiAction: true });
  host.appear("mic", settings); await key(host, "mic", 1, "#35D07F");
  const { pi } = await options(host, "mute");
  let deltasBeforeEnd = -1;
  server.once("windowEnd", () => { deltasBeforeEnd = server.sent.filter(m => m.type === "stateDelta").length; });
  host.keyDown("mute"); host.keyUp("mute"); host.keyDown("window"); host.keyUp("window");
  await key(host, "mic", 1, "#FF5A5F");
  await until(host, "pi", () => pi.messages.at(-1)?.payload.revision === server.snapshot.revision);
  await host.fence();
  assert.equal(deltasBeforeEnd, 0); assert.equal(server.snapshot.state.muteAll.mic, true);
  assert.deepEqual(commands(server).map(m => m.command), ["setMuteAll", "setPluginGroupWindows"]);
  assert.deepEqual(host.records.filter(r => r.context === "mute" || r.context === "window"), []);
  noFailures(server, host, ["mute", "window", "mic"]);
});

test("window wait: repeating the same target during its slow toggle is a silent no-op", async t => {
  const { server, host } = await setupActions(t); server.behavior.windowDelayMs = 1200;
  host.appear("window", {}, 0, "plugin-window"); await readyWindow(host, "window", "toggle");
  host.keyDown("window"); await nextEvent(server, "windowStart");
  host.keyDown("window"); host.keyUp("window"); await host.fence();
  await until(server, "command", () => server.sent.some(m => m.type === "ack" && m.result.open === true));
  await options(host, "window"); await host.fence();
  assert.equal(commands(server).length, 1); noFailures(server, host, ["window"]);
  assert.ok([...server.pluginWindows.values()].every(group => group.open === 1));
});

test("window wait: multiple mic inputs survive the 500ms queue lifetime while the editor is opening", async t => {
  const { server, host, settings } = await setupActions(t); server.behavior.windowDelayMs = 1200;
  host.appear("window", { mode: "open" }, 0, "plugin-window");
  host.appear("a", settings, 1); host.appear("b", settings, 2);
  await readyWindow(host, "window"); await key(host, "a", 1, "#35D07F"); await key(host, "b", 1, "#35D07F");
  host.keyDown("window"); await nextEvent(server, "windowStart");
  host.keyDown("a"); host.keyDown("b"); await host.fence();
  await until(server, "command", () => commands(server).length === 3);
  const { pi } = await options(host, "a");
  await until(host, "pi", () => pi.messages.at(-1)?.payload.revision === server.snapshot.revision);
  await key(host, "a", 1, "#35D07F"); await host.fence();
  assert.deepEqual(commands(server).map(m => m.command), ["setPluginGroupWindows", "toggleChannel", "toggleChannel"]);
  noFailures(server, host, ["window", "a", "b"]);
});

test("window wait: outstanding request and ping deadlines restart after the editor ACK, with no new pings during the wait", async t => {
  const { server, connection, disconnects } = await setupConnection(t, { request: 400, ping: 100, pong: 400, pluginWindows: 2000 }, 700);
  server.behavior.omitPong = true;
  await until(server, "message", () => server.received.some(m => m.type === "ping"));
  const ping = server.received.find(m => m.type === "ping"), client = [...server.clients][0];
  server.behavior.holdCommands = true;
  const state = connection.command({ command: "requestState", args: {} });
  await until(server, "message", () => commands(server, "requestState").length === 1);
  const request = commands(server, "requestState")[0]; server.behavior.holdCommands = false;
  let pingsAtEnd = 0;
  const reply = new Promise<void>(resolve => server.once("windowEnd", () => {
    pingsAtEnd = server.received.filter(m => m.type === "ping").length;
    const timer = setTimeout(() => {
      server.execute(client, request);
      server.send(client, { v: 1, type: "pong", id: ping.id, ...server.context() });
      server.behavior.omitPong = false; resolve();
    }, 200);
    t.after(() => clearTimeout(timer));
  }));
  await Promise.all([state, connection.command(openGroup()), reply]);
  assert.equal(pingsAtEnd, 1); assert.deepEqual(disconnects, []); assert.equal(connection.ready, true);
  await until(server, "message", () => server.received.filter(m => m.type === "ping").length > 1);
  assert.deepEqual(disconnects, []);
});

test("window wait: a queued state request keeps its order and its snapshot deadline pauses across the next editor", async t => {
  const { server, connection, disconnects } = await setupConnection(t, { request: 400, pluginWindows: 2000 }, 600);
  server.behavior.omitState = true;
  const first = connection.command(openGroup(1));
  await nextEvent(server, "windowStart");
  connection.requestState(); assert.equal(connection.ready, true, "A held state request does not invalidate the current snapshot yet");
  const second = connection.command(openGroup(2));
  const reply = new Promise<void>(resolve => server.on("windowEnd", m => {
    if (m.args.index !== 2) return;
    const request = commands(server, "requestState")[0], client = [...server.clients][0];
    const timer = setTimeout(() => { server.sendState(client, "requested", request.id); resolve(); }, 200);
    t.after(() => clearTimeout(timer));
  }));
  await Promise.all([first, second, reply]); await until(connection, "status", () => connection.ready);
  assert.deepEqual(server.received.filter(m => m.type === "command").map(m => m.command), ["setPluginGroupWindows", "requestState", "setPluginGroupWindows"]);
  assert.deepEqual(disconnects, []); assert.equal(server.connections, 1);
});

test("window wait: the configurable ten-second editor deadline disconnects, drops held input and reconnects without replay", async t => {
  const { server, connection, queue, channelId, disconnects } = await setupConnection(t, { pluginWindows: 200 }, 800);
  const failures = Promise.all([
    assert.rejects(queue.enqueue("plugin-window/all/1", "window", () => openGroup())),
    assert.rejects(queue.enqueue("mic/" + channelId, "mic", () => micOn(channelId))),
    assert.rejects(connection.command({ command: "requestState", args: {} }), /REQUEST_TIMEOUT/)
  ]);
  assert.deepEqual(await nextEvent(connection, "disconnect"), ["REQUEST_TIMEOUT"]); await failures;
  await until(connection, "status", () => connection.ready);
  await until(server, "command", () => server.sent.some(m => m.type === "ack" && m.result.open === true));
  assert.deepEqual(disconnects, ["REQUEST_TIMEOUT"]); assert.equal(server.connections, 2);
  assert.equal(commands(server).length, 1); assert.equal(commands(server, "requestState").length, 0);
  assert.equal(connection.pluginWindowsBusy, false);
});

test("window wait: an editor error releases held commands without retrying the window operation", async t => {
  const { server, connection, queue, channelId, disconnects } = await setupConnection(t, { request: 300, pluginWindows: 2000 });
  server.pluginWindows.set(channelId + "/1", { count: 0, open: 0 });
  const failedWindow = assert.rejects(queue.enqueue("plugin-window/all/1", "window", () => openGroup()), /PLUGIN_GROUP_EMPTY/);
  const mic = queue.enqueue("mic/" + channelId, "mic", () => micOn(channelId));
  await Promise.all([failedWindow, mic]);
  assert.deepEqual(commands(server).map(m => m.command), ["setPluginGroupWindows", "setChannelOn"]);
  assert.deepEqual(disconnects, []); assert.equal(connection.ready, true);
});

test("window wait: a computed send waits for the held conflict refresh before its one allowed retry", async t => {
  const { server, connection, queue, channelId, disconnects } = await setupConnection(t);
  const fxId = server.snapshot.state.fx[0].id;
  let edits = 0;
  server.on("message", m => {
    if (m.command === "setSend" && edits++ === 0) {
      server.snapshot.state.channels[0].sends[0].amount = 0.5; server.snapshot.revision++;
    }
  });
  await Promise.all([
    queue.enqueue("plugin-window/all/1", "first", () => openGroup(1)),
    queue.enqueue("send/" + channelId, "send", s => ({ command: "setSend", args: {
      channelId, fxId, amount: s.state.channels[0]!.sends[0]!.amount + 0.1
    } }), true, 1),
    queue.enqueue("plugin-window/all/2", "second", () => openGroup(2))
  ]);
  assert.deepEqual(commands(server, "setSend").map(m => m.ifRevision), [1, 2]);
  assert.deepEqual(commands(server, "setSend").map(m => m.args.amount), [0.1, 0.6]);
  assert.equal(connection.store.snapshot?.state.channels[0]!.sends[0]!.amount, 0.6);
  assert.deepEqual(disconnects, []); assert.equal(server.connections, 1);
});

test("window wait: a changed session discards held input and resynchronizes without replay", async t => {
  const { server, connection, queue, channelId, disconnects } = await setupConnection(t);
  const failures = Promise.all([
    assert.rejects(queue.enqueue("plugin-window/all/1", "window", () => openGroup())),
    assert.rejects(queue.enqueue("mic/" + channelId, "mic", () => micOn(channelId)))
  ]);
  await nextEvent(server, "windowStart");
  server.once("windowEnd", () => server.broadcastState("sessionChanged"));
  server.snapshot.sessionId = "cccccccccccc4ccc8ccccccccccccccc"; server.snapshot.revision++;
  await failures;
  await until(connection, "status", () => connection.ready && connection.store.snapshot?.sessionId === server.snapshot.sessionId);
  assert.deepEqual(commands(server).map(m => m.command), ["setPluginGroupWindows"]);
  assert.deepEqual(disconnects, []); assert.equal(connection.pluginWindowsBusy, false);
});
