import test from "node:test";
import assert from "node:assert/strict";
import type { Snapshot } from "../src/livemix/protocol.js";
import { strings } from "../src/ui/strings.js";
import { FakeHost } from "./fake-host.mjs";
import { until } from "./helpers.mjs";
import { setupActions, options, alerts, commands, title } from "./action-helpers.js";

const kinds = ["mic", "all-mics", "mic-mute-group", "fx-mute-group", "plugin-group", "plugin-group-all", "fx-send-step", "status"] as const;
const visualEvents = ["setState", "setImage", "setTitle"];
const assertSilent = (host: FakeHost, id: string): void => {
  assert.deepEqual(host.records.filter(r => r.context === id && visualEvents.includes(r.event)), [], id);
};

for (const kind of kinds) test(`${kind}: multi-action stays silent on appear/refresh/input while a normal key still renders`, async t => {
  const { server, host, settings } = await setupActions(t, "en");
  const configured = { ...settings, mode: kind === "fx-send-step" ? "up" : "toggle", display: "audio" };
  host.appear("normal", configured, 0, kind);
  host.appear("multi", configured, 0, kind, "Keypad", { isInMultiAction: true });
  const { pi } = await options(host, "multi");
  await until(host, "pi", () => pi.messages.at(-1)?.payload.connection === "ready");
  assert.equal(pi.messages.at(-1)!.payload.isInMultiAction, true);
  await host.wait(() => !!host.visual("normal").title && !/LiveMix|Checking/.test(title(host, "normal")));
  assert.deepEqual([...new Set(host.records.filter(r => r.context === "normal").map(r => r.event))].sort(), [...visualEvents].sort());
  assertSilent(host, "multi");
  const before = host.visual("normal");
  server.mutate((s: Snapshot) => {
    for (const channel of s.state.channels) {
      channel.on = false;
      for (const group of channel.pluginGroups) group.off = true;
      for (const send of channel.sends) send.amount = 0.6;
    }
    s.state.muteGroups = { mic: true, fx: true }; s.state.audio.running = false;
  }, true);
  await until(host, "pi", () => pi.messages.at(-1)?.payload.revision === server.snapshot.revision);
  await host.wait(() => host.visual("normal").image !== before.image);
  assert.equal(host.visual("normal").state, kind.endsWith("mute-group") ? 1 : 0);
  assertSilent(host, "multi");

  const expected = {
    mic: { command: "toggleChannel", args: { channelId: settings.channelId } },
    "all-mics": { command: "setAllChannelsOn", args: { on: true } },
    "mic-mute-group": { command: "toggleMuteGroup", args: { group: "mic" } },
    "fx-mute-group": { command: "toggleMuteGroup", args: { group: "fx" } },
    "plugin-group": { command: "setPluginGroupOff", args: { channelId: settings.channelId, index: 1, off: false } },
    "plugin-group-all": { command: "setPluginGroupOffEverywhere", args: { index: 1, off: false } },
    "fx-send-step": { command: "setSend", args: { channelId: settings.channelId, fxId: settings.fxId, amount: 0.61 } },
    status: { command: "requestState", args: {} }
  }[kind];
  host.keyDown("multi"); host.keyUp("multi");
  await until(server, "message", () => commands(server, expected.command).length === 1);
  assert.deepEqual(commands(server, expected.command).map(m => ({ command: m.command, args: m.args })), [expected]);
  await until(host, "pi", () => pi.messages.at(-1)?.payload.revision === server.snapshot.revision);
  await host.fence(); assertSilent(host, "multi"); assert.equal(alerts(host, "multi").length, 0);
  host.assertBudget();
});

for (const kind of ["mic", "all-mics", "plugin-group", "plugin-group-all"] as const) test(`${kind}: multi-action executes toggle/on/off settings regardless of userDesiredState`, async t => {
  const { server, host, settings } = await setupActions(t);
  host.appear("multi", settings, 0, kind, "Keypad", { isInMultiAction: true });
  const { pi } = await options(host, "multi");
  await until(host, "pi", () => pi.messages.at(-1)?.payload.connection === "ready");
  const expected: { command: string; args: object }[] = [];
  for (const [mode, userDesiredState, on] of [["toggle", 1, false], ["on", 0, true], ["off", 1, false], ["toggle", 0, true], ["toggle", 1, false]] as const) {
    host.settings("multi", { ...settings, mode });
    host.keyDown("multi", { userDesiredState }); host.keyUp("multi");
    expected.push(kind === "mic" ? mode === "toggle"
      ? { command: "toggleChannel", args: { channelId: settings.channelId } }
      : { command: "setChannelOn", args: { channelId: settings.channelId, on } }
      : kind === "all-mics" ? { command: "setAllChannelsOn", args: { on } }
      : { command: kind === "plugin-group" ? "setPluginGroupOff" : "setPluginGroupOffEverywhere",
        args: { ...(kind === "plugin-group" ? { channelId: settings.channelId } : {}), index: 1, off: !on } });
    await until(server, "command", () => commands(server).length === expected.length);
    await until(host, "pi", () => pi.messages.at(-1)?.payload.revision === server.snapshot.revision);
    assert.deepEqual(commands(server).map(m => ({ command: m.command, args: m.args })), expected);
    assertSilent(host, "multi");
  }
  await host.fence(); assert.equal(commands(server).length, 5); assert.equal(alerts(host, "multi").length, 0);
});

for (const kind of ["mic", "plugin-group"] as const) test(`${kind}: missing binding and disconnection still alert in a multi-action without visual output`, async t => {
  const { server, host, settings } = await setupActions(t);
  host.appear("multi", settings, 0, kind, "Keypad", { isInMultiAction: true });
  const { pi } = await options(host, "multi");
  await until(host, "pi", () => pi.messages.at(-1)?.payload.connection === "ready");
  server.mutate((s: Snapshot) => { s.state.channels = s.state.channels.filter(c => c.id !== settings.channelId); }, true);
  await until(host, "pi", () => pi.messages.at(-1)?.payload.revision === server.snapshot.revision);
  assert.equal(alerts(host, "multi").length, 0);
  host.keyDown("multi"); await host.wait(() => alerts(host, "multi").length === 1);
  await until(host, "pi", () => pi.messages.at(-1)?.payload.message === strings.ko.missing);
  assertSilent(host, "multi"); assert.equal(commands(server).length, 0);
  await server.setStatus("stopped");
  await until(host, "pi", () => pi.messages.at(-1)?.payload.connection === "disconnected");
  host.keyDown("multi"); await host.wait(() => alerts(host, "multi").length === 2);
  assertSilent(host, "multi"); assert.equal(commands(server).length, 0); host.assertBudget();
});

for (const language of ["ko", "en"] as const) test(`${language}: mic and shared-base PI options identify multi-actions and keep the flag while offline`, async t => {
  const { server, host, settings } = await setupActions(t, language);
  for (const kind of ["mic", "plugin-group"]) for (const isInMultiAction of [false, true]) {
    const id = `${kind}-${isInMultiAction}`;
    host.appear(id, settings, 0, kind, "Keypad", { isInMultiAction });
    const { pi } = await options(host, id);
    await until(host, "pi", () => pi.messages.at(-1)?.payload.connection === "ready");
    const dto = pi.messages.at(-1)!.payload;
    assert.equal(dto.isInMultiAction, isInMultiAction); assert.equal(dto.language, language);
    assert.equal(dto.message, strings[language].connected); assert.equal(dto.settings.mode, "toggle");
    await host.closeInspector(id);
  }
  await server.setStatus("stopped");
  for (const kind of ["mic", "plugin-group"]) {
    const id = `${kind}-true`, { pi } = await options(host, id);
    await until(host, "pi", () => pi.messages.at(-1)?.payload.connection === "disconnected");
    assert.equal(pi.messages.at(-1)!.payload.isInMultiAction, true);
    assert.equal(pi.messages.at(-1)!.payload.language, language);
    assert.equal(pi.messages.at(-1)!.payload.message, strings[language].offlineHelp);
    assertSilent(host, id); await host.closeInspector(id);
  }
});
