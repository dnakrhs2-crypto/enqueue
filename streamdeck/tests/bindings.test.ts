import test from "node:test";
import assert from "node:assert/strict";
import { MicBinding, FxBinding, migrateSettings, actionSettings } from "../src/livemix/bindings.js";
import { validateServerMessage, type Snapshot } from "../src/livemix/protocol.js";
import { fixture } from "./helpers.mjs";

const origin = "11111111111141118111111111111111", other = "22222222222242228222222222222222", third = "33333333333343338333333333333333";
const settings = (patch = {}) => migrateSettings({ channelId: origin, channelName: "마이크 1", ...patch }).settings;
async function state() { return validateServerMessage(await fixture("initial-state")) as Snapshot; }
const nextSession = (s: Snapshot): Snapshot => ({ ...s, sessionId: "cccccccccccc4ccc8ccccccccccccccc", revision: s.revision + 1 });
function boundId(binding: MicBinding, s: Snapshot) { const result = binding.resolve(s); return result.status === "bound" ? result.channel.id : result.status; }

test("legacy settings migrate to settingsVersion:1; repeated migration is a no-op", () => {
  const first = migrateSettings({ channelId: origin, channelName: "곰 마이크" });
  assert.equal(first.valid, true); assert.equal(first.changed, true); assert.equal(first.settings.settingsVersion, 1);
  assert.equal(first.settings.mode, "toggle"); assert.equal(first.settings.nameFallback, true);
  assert.equal(migrateSettings(first.settings).changed, false);
});

test("FX binding uses independent UUID/name resolution and preserves deletion boundaries and origin", async () => {
  const s = await state(), saved = { fxId: other, fxName: "리버브", nameFallback: true }, binding = new FxBinding(saved);
  assert.equal(binding.resolve(s).status, "bound");
  s.state.fx[0]!.id = third;
  assert.equal(binding.resolve(s).status, "missing");
  const next = nextSession(s), result = binding.resolve(next);
  assert.equal(result.status === "bound" && result.fx.id, third); assert.equal(saved.fxId, other);
  next.state.fx[0]!.name = "Temporary"; assert.equal(binding.resolve(next).status, "bound");
  assert.equal(binding.resolve({ ...next, sessionId: "dddddddddddd4ddd8ddddddddddddddd" }).status, "missing");
});

test("FX duplicate names require reselection, while original rename updates future fallback", async () => {
  const s = await state(), settings = { fxId: other, fxName: "리버브", nameFallback: true }, binding = new FxBinding(settings);
  s.state.fx[0]!.name = "새 리버브";
  const renamed = binding.resolve(s); assert.equal(renamed.status === "bound" && renamed.renamedOrigin, "새 리버브");
  s.state.fx[0]!.id = third; assert.equal(binding.resolve(nextSession(s)).status, "bound");
  s.state.fx.push({ ...s.state.fx[0]!, id: origin });
  assert.equal(new FxBinding({ ...settings, fxName: "새 리버브" }).resolve(s).status, "duplicate");
});

test("Round 3 settings validate action-specific enum/index/step bounds and supply safe defaults", () => {
  for (const [kind, bad] of [["plugin-group", { groupIndex: 0 }], ["fx-send", { stepPercent: 2 }], ["fx-send", { pressMode: "reset" }],
    ["status", { display: "start" }], ["mic-mute-group", { mode: "on" }], ["fx-mute-group", { mode: "off" }], ["all-mics", { mode: "mute" }]] as const) {
    assert.equal(actionSettings(kind, bad).valid, false);
  }
  const defaults = actionSettings("fx-send", {}); assert.equal(defaults.settings.stepPercent, 1); assert.equal(defaults.settings.pressMode, "pre-post");
  assert.equal(actionSettings("fx-send", defaults.settings).changed, false); assert.equal(actionSettings("status", {}).settings.display, "connection");
});
test("invalid enum/future settings versions disable input; malformed UUID does not select a channel", () => {
  for (const raw of [{ mode: "TOGGLE" }, { settingsVersion: 2 }, { nameFallback: "true" }]) assert.equal(migrateSettings(raw).valid, false);
  assert.equal(migrateSettings({ channelId: "../session" }).settings.channelId, "");
});

test("plugin-group-all settings validate numbered slots and modes without a channel binding", () => {
  const defaults = actionSettings("plugin-group-all", {});
  assert.equal(defaults.valid, true); assert.equal(defaults.settings.groupIndex, 1); assert.equal(defaults.settings.mode, "toggle");
  assert.equal(defaults.settings.channelId, ""); assert.equal(actionSettings("plugin-group-all", defaults.settings).changed, false);
  for (const kind of ["plugin-group", "plugin-group-all"] as const) {
    for (const groupIndex of [1, 2, 3, 4, 5]) for (const mode of ["toggle", "on", "off"]) assert.equal(actionSettings(kind, { groupIndex, mode }).valid, true);
    for (const groupIndex of [0, 6, 1.5, "2", NaN, Infinity, null]) assert.equal(actionSettings(kind, { groupIndex }).valid, false);
  }
  assert.equal(actionSettings("plugin-group-all", { mode: "mute" }).valid, false);
});

test("send key settings default to up/5/50 and reject invalid modes, steps and target percentages", () => {
  const defaults = actionSettings("fx-send-step", {});
  assert.equal(defaults.valid, true); assert.equal(defaults.settings.mode, "up");
  assert.equal(defaults.settings.stepPercent, 5); assert.equal(defaults.settings.targetPercent, 50);
  assert.equal(actionSettings("fx-send-step", defaults.settings).changed, false);
  for (const mode of ["up", "down", "set"]) for (const stepPercent of [1, 5, 10]) for (const targetPercent of [0, 50, 100]) {
    assert.equal(actionSettings("fx-send-step", { mode, stepPercent, targetPercent }).valid, true);
  }
  for (const bad of [{ mode: "toggle" }, { stepPercent: 2 }, { stepPercent: "5" }, { targetPercent: -1 }, { targetPercent: 101 },
    { targetPercent: 12.5 }, { targetPercent: "50" }, { targetPercent: NaN }, { targetPercent: Infinity }]) {
    assert.equal(actionSettings("fx-send-step", bad).valid, false);
  }
  assert.equal(actionSettings("fx-send", { stepPercent: 10 }).valid, false);
});
test("UUID wins over duplicate names and misleading short titles", async () => {
  const s = await state(), binding = new MicBinding(settings({ shortTitle: "another mic" }));
  s.state.channels.push({ ...s.state.channels[0]!, id: other }); assert.equal(boundId(binding, s), origin);
});
test("unique exact-name fallback is allowed on initial/new-session full state without overwriting origin", async () => {
  const s = await state(), saved = settings(), binding = new MicBinding(saved); s.state.channels[0]!.id = other;
  assert.equal(boundId(binding, s), other); assert.equal(saved.channelId, origin);
  const returned = nextSession(s); returned.state.channels.push({ ...s.state.channels[0]!, id: origin }); assert.equal(boundId(binding, returned), origin);
});
test("case changes, substrings, trimmed names and fallback OFF never match", async () => {
  for (const patch of [{ channelName: "마이크" }, { channelName: "마이크 1 " }, { nameFallback: false }, { channelName: "GOM" }]) {
    const s = await state(); s.state.channels[0]!.id = other; assert.equal(boundId(new MicBinding(settings(patch)), s), "missing");
  }
});
test("duplicate exact names require reselection and never choose the first entry", async () => {
  const s = await state(); s.state.channels[0]!.id = other; s.state.channels.push({ ...s.state.channels[0]!, id: third });
  assert.equal(boundId(new MicBinding(settings()), s), "duplicate");
});
test("origin rename updates fallback name for the next session", async () => {
  const s = await state(), binding = new MicBinding(settings()); s.state.channels[0]!.name = "New name";
  const result = binding.resolve(s); assert.equal(result.status === "bound" && result.renamedOrigin, "New name");
  const next = nextSession(s); next.state.channels[0]!.id = other; assert.equal(boundId(binding, next), other);
});
test("temporary fallback rename does not change origin's saved fallback name", async () => {
  const s = await state(), binding = new MicBinding(settings()); s.state.channels[0]!.id = other; binding.resolve(s);
  s.state.channels[0]!.name = "Temporary rename"; const renamed = binding.resolve(s); assert.equal(renamed.status === "bound" && renamed.renamedOrigin, undefined);
  assert.equal(boundId(binding, nextSession(s)), "missing");
});
test("deleting origin in the same session never binds a same-name replacement", async () => {
  const s = await state(), binding = new MicBinding(settings()); binding.resolve(s);
  s.state.channels[0]!.id = other; assert.equal(boundId(binding, s), "missing");
  s.revision++; assert.equal(boundId(binding, s), "missing");
  assert.equal(boundId(binding, nextSession(s)), other);
});
test("deleting a temporary fallback also latches missing for that session", async () => {
  const s = await state(), binding = new MicBinding(settings()); s.state.channels[0]!.id = other; binding.resolve(s);
  s.state.channels[0]!.id = third; assert.equal(boundId(binding, s), "missing"); assert.equal(boundId(binding, nextSession(s)), third);
});
test("mode/title updates and same-session reconnect snapshots do not unlock deletion fallback", async () => {
  const s = await state(), binding = new MicBinding(settings()); binding.resolve(s); s.state.channels[0]!.id = other; binding.resolve(s);
  binding.update(settings({ mode: "off", shortTitle: "Alias" })); assert.equal(boundId(binding, structuredClone(s)), "missing");
  binding.update(settings({ nameFallback: false })); binding.update(settings({ nameFallback: true }));
  assert.equal(boundId(binding, s), "missing");
});
test("explicit PI target selection and new instance re-evaluate binding", async () => {
  const s = await state(), binding = new MicBinding(settings()); binding.resolve(s); s.state.channels[0]!.id = other;
  assert.equal(boundId(binding, s), "missing"); binding.update(settings({ channelId: other })); assert.equal(boundId(binding, s), other);
  const automatic = new MicBinding(settings()); automatic.resolve(await state());
  assert.equal(boundId(automatic, { ...s, instanceId: "dddddddddddd4ddd8ddddddddddddddd" }), other);
});
