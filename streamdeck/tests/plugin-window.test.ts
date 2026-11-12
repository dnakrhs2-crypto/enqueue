import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { resolve } from "node:path";
import { actionSettings } from "../src/livemix/bindings.js";
import { LiveMixConnection } from "../src/livemix/connection.js";
import { CommandQueue } from "../src/livemix/command-queue.js";
import { validAck, type Ack, type Command, type Snapshot } from "../src/livemix/protocol.js";
import { displayName, statusTitle, windowImage } from "../src/ui/key-renderer.js";
import { windowSvg } from "../src/ui/artwork.js";
import { strings } from "../src/ui/strings.js";
import { FakeLiveMix } from "./fake-livemix-server.mjs";
import type { FakeHost } from "./fake-host.mjs";
import { fixture, pluginRoot, until } from "./helpers.mjs";
import { setupActions, options, commands, alerts } from "./action-helpers.js";

const kind = "plugin-window", uuid = "com.gomtwigim.livemix.plugin-window";
const missingId = "99999999999949998999999999999999";
const acks = (server: FakeLiveMix): Ack[] => server.sent.filter(m => m.type === "ack" && "open" in m.result);

async function acknowledged(server: FakeLiveMix, host: FakeHost): Promise<void> {
  // A status request uses the same TCP gate, so its arrival proves the plugin consumed the window ACK.
  const count = commands(server, "requestState").length, id = `ack-${count}`;
  host.appear(id, {}, 0, "status", "Keypad", { isInMultiAction: true }); host.keyDown(id);
  await until(server, "message", () => commands(server, "requestState").length === count + 1);
  await host.disappear(id);
}

test("plugin-window settings default to all/1/toggle, preserve bindings and reject invalid settings without rewriting them", () => {
  for (const legacy of [{}, { channelId: missingId, channelName: "마이크", shortTitle: "그룹 창", nameFallback: false }]) {
    const parsed = actionSettings(kind, legacy);
    assert.equal(parsed.valid, true); assert.equal(parsed.changed, true);
    assert.equal(parsed.settings.windowTarget, "all"); assert.equal(parsed.settings.groupIndex, 1); assert.equal(parsed.settings.mode, "toggle");
    assert.equal(parsed.settings.settingsVersion, 1);
    for (const [key, value] of Object.entries(legacy)) assert.equal(parsed.settings[key as keyof typeof parsed.settings], value);
    assert.equal(actionSettings(kind, parsed.settings).changed, false);
  }
  for (const windowTarget of ["all", "mic"]) for (const groupIndex of [1, 2, 3, 4, 5]) for (const mode of ["toggle", "open", "close"]) {
    const parsed = actionSettings(kind, { windowTarget, groupIndex, mode });
    assert.equal(parsed.valid, true); assert.equal(parsed.settings.windowTarget, windowTarget);
    assert.equal(parsed.settings.groupIndex, groupIndex); assert.equal(parsed.settings.mode, mode);
  }
  for (const bad of [{ windowTarget: "ALL" }, { windowTarget: null }, { mode: "on" }, { mode: "off" }, { mode: "mute" },
    { groupIndex: 0 }, { groupIndex: 6 }, { groupIndex: 1.5 }, { groupIndex: "2" }, { settingsVersion: 2 }, { nameFallback: "true" }]) {
    const parsed = actionSettings(kind, bad); assert.equal(parsed.valid, false); assert.equal(parsed.changed, false);
  }
  assert.equal(actionSettings(kind, { windowTarget: "mic", channelId: "bad-uuid" }).settings.channelId, "");
  assert.equal(actionSettings("plugin-group", {}).settings.windowTarget, undefined);
  assert.equal(actionSettings("plugin-group-all", { mode: "open" }).valid, false);
});

test("plugin-window manifest and static previews use the supplied translations and window artwork", async () => {
  const manifest = JSON.parse(await readFile(resolve(pluginRoot, "manifest.json"), "utf8"));
  const action = manifest.Actions.find((a: any) => a.UUID === uuid);
  assert.equal(action.Name, strings.en.pluginWindow); assert.equal(action.Tooltip, strings.en.pluginWindowTooltip);
  assert.equal(action.States.length, 1); assert.equal(action.States[0].Name, strings.en.windowToggle);
  assert.deepEqual(action.Controllers, ["Keypad"]); assert.equal(action.SupportedInMultiActions, true);
  for (const language of ["ko", "en"] as const) {
    const locale = JSON.parse(await readFile(resolve(pluginRoot, language + ".json"), "utf8"));
    assert.equal(locale[uuid].Name, strings[language].pluginWindow); assert.equal(locale[uuid].Tooltip, strings[language].pluginWindowTooltip);
  }
  for (const [suffix, size] of [["", 72], ["@2x", 144]] as const) {
    const read = async (name: string): Promise<string> => (await readFile(resolve(pluginRoot, "imgs/actions/plugin-window/" + name + suffix + ".svg"), "utf8")).trim();
    assert.equal(await read("off"), windowSvg("toggle", true, 1, strings.en, false, size));
    for (const all of [true, false]) for (const mode of ["toggle", "open", "close"] as const) for (const index of [1, 2, 3, 4, 5]) {
      assert.equal(await read(`group-${index}-${all ? "all" : "mic"}-${mode}`), windowSvg(mode, all, index, strings.en, false, size));
    }
  }
});

test("plugin-window ACK validates both commands, unchanged projection and a nonnegative window count", async () => {
  const snapshot = await fixture("initial-state");
  for (const row of await fixture("plugin-windows") as { command: Command; result: Ack["result"] }[]) {
    const ack: Ack = { v: 1, type: "ack", id: "2", instanceId: snapshot.instanceId, sessionId: snapshot.sessionId, revision: 1, changed: false, result: row.result };
    assert.equal(validAck(row.command, ack), true);
    assert.equal(validAck(row.command, { ...ack, changed: true }), false);
    assert.equal(validAck(row.command, { ...ack, result: { ...row.result, count: 0 } }), true);
    for (const bad of [{}, { ...row.result, index: 2 }, { ...row.result, open: "true" }, { ...row.result, count: -1 }, { ...row.result, count: 1.5 }, { ...row.result, count: "1" }]) {
      assert.equal(validAck(row.command, { ...ack, result: bad }), false);
    }
  }
});

for (const language of ["ko", "en"] as const) for (const windowTarget of ["all", "mic"] as const)
test(`plugin-window ${language}/${windowTarget}: toggle/open/close, repeat presses, default title and audio badge without state changes`, async t => {
  const { server, host, settings } = await setupActions(t, language);
  const all = windowTarget === "all", groupIndex = 2;
  const configured = { ...settings, windowTarget, groupIndex };
  host.appear("key", configured, 0, kind);
  const expectedTitle = displayName(`${all ? strings[language].allMicsShort : settings.channelName} · ${strings[language].group} ${groupIndex}`);
  await host.wait(() => host.visual("key").title === expectedTitle && host.visual("key").image === windowImage("toggle", all, groupIndex, language));
  const before = structuredClone(server.snapshot), modes = ["toggle", "toggle", "open", "open", "close", "close"] as const;
  for (const [i, mode] of modes.entries()) {
    host.settings("key", { ...configured, mode });
    host.keyDown("key"); host.keyUp("key");
    await until(server, "command", () => commands(server).length === i + 1);
    await acknowledged(server, host); // Intentional successive presses; the slow-editor test covers in-flight repeats.
    await host.wait(() => host.visual("key").image === windowImage(mode, all, groupIndex, language));
    const sent = commands(server)[i], args = { index: groupIndex, ...(all ? {} : { channelId: settings.channelId }) };
    assert.equal(sent.command, mode === "toggle" ? "togglePluginGroupWindows" : "setPluginGroupWindows");
    assert.deepEqual(sent.args, mode === "toggle" ? args : { ...args, open: mode === "open" });
    assert.equal(sent.ifRevision, undefined); assert.equal(sent.instanceId, before.instanceId); assert.equal(sent.sessionId, before.sessionId);
    const ack = acks(server)[i]!;
    assert.equal(ack.changed, false); assert.equal(ack.revision, before.revision);
    assert.deepEqual(ack.result, { index: groupIndex, open: [true, false, true, true, false, false][i], count: i === 5 ? 0 : all ? 3 : 1 });
    assert.equal(host.visual("key").state, 0); assert.equal(host.visual("key").title, expectedTitle);
  }
  await host.fence(); assert.equal(commands(server).length, 6); assert.equal(alerts(host, "key").length, 0);
  assert.deepEqual(server.snapshot, before); assert.equal(server.sent.filter(m => m.type === "stateDelta").length, 0);
  assert.equal(commands(server, "requestState").length, modes.length, "Only the explicit ACK probes request state");
  server.mutate((s: Snapshot) => { s.state.audio.running = false; });
  host.settings("key", { ...configured, mode: "close", shortTitle: "방송 플러그인" });
  await host.wait(() => host.visual("key").title === displayName("방송 플러그인") && host.visual("key").image === windowImage("close", all, groupIndex, language, true));
  assert.equal(host.visual("key").state, 0);
});

for (const language of ["ko", "en"] as const) test(`plugin-window ${language}: unsupported capability alerts without sending a command`, async t => {
  const { server, host } = await setupActions(t, language, { pluginGroupWindows: false });
  host.appear("key", {}, 0, kind);
  await host.wait(() => host.visual("key").title === statusTitle(strings[language].unsupported));
  assert.equal(host.visual("key").state, 0); host.keyDown("key"); host.keyUp("key");
  await host.wait(() => alerts(host, "key").length === 1);
  const { pi } = await options(host, "key"); assert.equal(pi.messages.at(-1)!.payload.message, strings[language].unsupported);
  await host.fence(); assert.equal(commands(server).length, 0);
});

for (const language of ["ko", "en"] as const) test(`plugin-window ${language}: missing groups/mic and invalid settings render errors and never send`, async t => {
  const { server, host, settings } = await setupActions(t, language);
  for (const [i, [patch, error]] of [
    [{ windowTarget: "all", groupIndex: 5 }, "missingGroup"],
    [{ windowTarget: "mic", groupIndex: 5 }, "missingGroup"],
    [{ windowTarget: "mic", channelId: missingId, nameFallback: false }, "missing"],
    [{ windowTarget: "elsewhere" }, "invalidSettings"], [{ groupIndex: 6 }, "invalidSettings"], [{ mode: "on" }, "invalidSettings"]
  ].entries()) {
    const id = String(i); host.appear(id, { ...settings, ...patch as object }, i, kind);
    await host.wait(() => host.visual(id).title === statusTitle(strings[language][error as "missingGroup" | "missing" | "invalidSettings"]));
    host.keyDown(id); host.keyUp(id); await host.wait(() => alerts(host, id).length === 1);
    assert.equal(host.visual(id).state, 0);
  }
  server.mutate((s: Snapshot) => { s.state.channels = []; }, true);
  await host.wait(() => host.visual("0").title === statusTitle(strings[language].missingGroup));
  await host.fence(); assert.equal(commands(server).length, 0);
});

for (const language of ["ko", "en"] as const) test(`plugin-window ${language}: empty groups alert and show the translated PI message without retry`, async t => {
  const { server, host, settings } = await setupActions(t, language);
  for (const channel of server.snapshot.state.channels) server.pluginWindows.set(`${channel.id}/1`, { count: 0, open: 0 });
  for (const windowTarget of ["all", "mic"]) {
    host.appear(windowTarget, { ...settings, windowTarget }, 0, kind);
    await host.wait(() => host.visual(windowTarget).image === windowImage("toggle", windowTarget === "all", 1, language));
    const { pi } = await options(host, windowTarget);
    host.keyDown(windowTarget); host.keyUp(windowTarget);
    await host.wait(() => alerts(host, windowTarget).length === 1);
    await until(host, "pi", () => pi.messages.at(-1)?.payload.message === strings[language].emptyGroup);
    assert.equal(host.visual(windowTarget).state, 0); await host.closeInspector(windowTarget);
  }
  await host.fence(); assert.equal(commands(server).length, 2); assert.equal(commands(server, "requestState").length, 0);
  assert.deepEqual(server.sent.filter(m => m.type === "error").map(m => [m.code, m.retryable]), [["PLUGIN_GROUP_EMPTY", false], ["PLUGIN_GROUP_EMPTY", false]]);
});

for (const windowTarget of ["all", "mic"]) test(`plugin-window ${windowTarget}: multi-action executes all three modes without visual output`, async t => {
  const { server, host, settings } = await setupActions(t);
  host.appear("multi", { ...settings, windowTarget }, 0, kind, "Keypad", { isInMultiAction: true });
  const { pi } = await options(host, "multi");
  await until(host, "pi", () => pi.messages.at(-1)?.payload.connection === "ready");
  assert.equal(pi.messages.at(-1)!.payload.isInMultiAction, true);
  for (const [i, mode] of ["toggle", "open", "close"].entries()) {
    host.settings("multi", { ...settings, windowTarget, mode });
    host.keyDown("multi", { userDesiredState: mode === "close" ? 1 : 0 }); host.keyUp("multi");
    await until(server, "command", () => commands(server).length === i + 1);
    await acknowledged(server, host);
    const sent = commands(server)[i], args = { index: 1, ...(windowTarget === "mic" ? { channelId: settings.channelId } : {}) };
    assert.equal(sent.command, mode === "toggle" ? "togglePluginGroupWindows" : "setPluginGroupWindows");
    assert.deepEqual(sent.args, mode === "toggle" ? args : { ...args, open: mode === "open" });
  }
  server.mutate((s: Snapshot) => { s.state.audio.running = false; });
  await until(host, "pi", () => pi.messages.at(-1)?.payload.revision === server.snapshot.revision);
  await host.fence(); assert.equal(commands(server).length, 3); assert.equal(commands(server, "requestState").length, 3);
  assert.deepEqual(host.records.filter(r => r.context === "multi"), []);
});

test("plugin-window one mic keeps origin UUID/name rules across rename, fallback and duplicate names", async t => {
  const { server, host, settings } = await setupActions(t), name = "방송 마이크", nextId = "cccccccccccc4ccc8ccccccccccccccc";
  host.appear("key", { ...settings, windowTarget: "mic" }, 0, kind);
  await host.wait(() => host.visual("key").image === windowImage("toggle", false, 1, "ko"));
  const { pi } = await options(host, "key");
  server.mutate((s: Snapshot) => { s.state.channels[0]!.name = name; });
  await until(host, "pi", () => pi.messages.some(m => m.event === "didReceiveSettings" && m.payload.settings.channelName === name));
  server.mutate((s: Snapshot) => { s.sessionId = "dddddddddddd4ddd8ddddddddddddddd"; s.state.channels[0]!.id = nextId; }, true);
  await until(host, "pi", () => pi.messages.at(-1)?.payload.sessionId === server.snapshot.sessionId);
  host.keyDown("key"); await until(server, "command", () => commands(server).length === 1);
  assert.equal(commands(server)[0].args.channelId, nextId); assert.equal(host.contexts.get("key")!.settings.channelId, settings.channelId);
  assert.equal(host.contexts.get("key")!.settings.channelName, name);
  server.mutate((s: Snapshot) => { s.sessionId = "eeeeeeeeeeee4eee8eeeeeeeeeeeeeee"; s.state.channels[1]!.name = name; }, true);
  await host.wait(() => host.visual("key").title === statusTitle(strings.ko.duplicate));
  host.keyDown("key"); await host.wait(() => alerts(host, "key").length === 1); assert.equal(commands(server).length, 1);
});

test("plugin-window queue completes both ACKs before any unrelated newer projection is published", async t => {
  const server = await new FakeLiveMix().start();
  server.snapshot.state.channels[0].pluginGroups = [{ index: 1, off: false }];
  const connection = new LiveMixConnection({ discoveryPath: server.discoveryPath }), queue = new CommandQueue(connection);
  t.after(async () => { queue.dispose(); connection.stop(); await server.close(); });
  connection.start(); await until(connection, "status", () => connection.ready);
  // An independent edit has not reached the observer yet. Window ACKs still finish immediately.
  server.snapshot.state.session.dirty = false; server.snapshot.revision++;
  await queue.enqueue("plugin-window/all/1", "a", () => ({ command: "togglePluginGroupWindows", args: { index: 1 } }));
  await queue.enqueue("plugin-window/all/1", "b", () => ({ command: "setPluginGroupWindows", args: { index: 1, open: false } }));
  assert.equal(connection.store.snapshot?.revision, 1); assert.equal(server.snapshot.revision, 2);
  assert.equal(acks(server).length, 2); assert.equal(commands(server, "requestState").length, 0);
  assert.ok(commands(server).every(m => m.ifRevision === undefined));
});

test("plugin-window fake wire matches the agreed results and reports missing/empty/busy/unsupported errors", async t => {
  const { server, host, settings } = await setupActions(t);
  const connection = new LiveMixConnection({ discoveryPath: server.discoveryPath }); t.after(() => connection.stop());
  connection.start(); await until(connection, "status", () => connection.ready);
  const before = structuredClone(server.snapshot);
  for (const row of await fixture("plugin-windows") as { command: Command; result: Ack["result"] }[]) {
    const ack = await connection.command(row.command); assert.equal(ack.changed, false); assert.equal(ack.revision, before.revision);
    assert.deepEqual(ack.result, row.result);
  }
  assert.deepEqual(server.snapshot, before);
  await assert.rejects(connection.command({ command: "togglePluginGroupWindows", args: { index: 1, channelId: missingId } }), /CHANNEL_NOT_FOUND/);
  await until(connection, "status", () => connection.ready);
  await assert.rejects(connection.command({ command: "togglePluginGroupWindows", args: { index: 5 } }), /PLUGIN_GROUP_NOT_FOUND/);
  await until(connection, "status", () => connection.ready);
  server.pluginWindows.set(`${settings.channelId}/1`, { count: 0, open: 0 });
  await assert.rejects(connection.command({ command: "setPluginGroupWindows", args: { index: 1, channelId: settings.channelId, open: true } }), /PLUGIN_GROUP_EMPTY/);
  server.behavior.sessionLoading = true;
  host.appear("busy", {}, 0, kind); await host.wait(() => host.visual("busy").image === windowImage("toggle", true, 1, "ko"));
  const count = commands(server).length; host.keyDown("busy"); host.keyUp("busy"); await host.wait(() => alerts(host, "busy").length === 1);
  await host.fence(); assert.equal(commands(server).length, count + 1);
  assert.equal(server.sent.findLast(m => m.type === "error")?.retryable, true);
  assert.equal(server.sent.findLast(m => m.type === "error")?.code, "SERVER_BUSY");
  server.behavior.sessionLoading = false; server.pluginGroupWindows = false;
  await assert.rejects(connection.command({ command: "togglePluginGroupWindows", args: { index: 1 } }), /UNKNOWN_COMMAND/);
});
