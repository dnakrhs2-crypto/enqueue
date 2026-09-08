import test from "node:test";
import assert from "node:assert/strict";
import { MicBinding, migrateSettings } from "../src/livemix/bindings.js";
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
test("invalid enum/future settings versions disable input; malformed UUID does not select a channel", () => {
  for (const raw of [{ mode: "TOGGLE" }, { settingsVersion: 2 }, { nameFallback: "true" }]) assert.equal(migrateSettings(raw).valid, false);
  assert.equal(migrateSettings({ channelId: "../session" }).settings.channelId, "");
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
