// Run the built plugin under the fake Stream Deck host against REAL LiveMix (External Control ON).
// An independent TCP observer checks mic, all-mics group, FX and all-mute toggles; original values are restored.
// Usage from streamdeck/: node tools/real_livemix_check.mjs
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { resolve } from "node:path";
import net from "node:net";
import { FakeHost } from "../tests/fake-host.mjs";
import { actionSvg, micSvg } from "../src/ui/artwork.ts";
import { micMuted, fxMuted } from "../src/livemix/muted.ts";
import { locales } from "./locales.mjs";

const appdata = process.env.APPDATA;
const discovery = JSON.parse(await readFile(resolve(appdata, "LiveMix/control/discovery.json"), "utf8"));
assert.equal(discovery.state, "ready", "LiveMix external control must be ready");
assert.equal(discovery.host, "127.0.0.1", "LiveMix control must use loopback");

const lines = [], host = new FakeHost();
let buffer = "", snapshot, observerIssue, sequence = 1;
const observer = net.createConnection({ host: discovery.host, port: discovery.port });
observer.setEncoding("utf8");
observer.on("error", () => { observerIssue = new Error("Observer connection failed"); });
observer.on("end", () => { observerIssue = new Error("Observer disconnected"); });
observer.on("data", chunk => {
  buffer += chunk;
  let i;
  while ((i = buffer.indexOf("\n")) >= 0) {
    const m = JSON.parse(buffer.slice(0, i)); buffer = buffer.slice(i + 1); lines.push(m);
    if (m.type === "state") snapshot = { ...m, state: { ...m.state, muteAll: { mic: false, fx: false, ...m.state.muteAll } } };
    else if (m.type === "stateDelta") {
      if (!snapshot || m.instanceId !== snapshot.instanceId || m.sessionId !== snapshot.sessionId || m.baseRevision !== snapshot.revision) {
        observerIssue = new Error("Observer state needs resynchronization"); continue;
      }
      for (const key of ["session", "audio", "muteGroups", "muteAll"]) if (m.changes[key]) Object.assign(snapshot.state[key], m.changes[key]);
      for (const key of ["channels", "fx"]) for (const patch of m.changes[key] || []) {
        const item = snapshot.state[key].find(item => item.id === patch.id);
        if (item) Object.assign(item, patch); else observerIssue = new Error("Observer received an unknown target");
      }
      snapshot.revision = m.revision;
    }
  }
});
const waitObserver = (predicate, ms = 6000) => new Promise((res, rej) => {
  const start = Date.now();
  const tick = () => {
    if (observerIssue) return rej(observerIssue);
    const value = predicate(); if (value) return res(value);
    if (Date.now() - start > ms) return rej(new Error("Observer timeout"));
    setTimeout(tick, 50);
  };
  tick();
});
const svg = image => image?.startsWith("data:image/svg+xml;base64,") ? Buffer.from(image.split(",")[1], "base64").toString("utf8") : "";
const groupTargets = () => snapshot.state.channels.flatMap(c => c.pluginGroups.filter(g => g.index === 1).map(g => ({ channelId: c.id, off: g.off })));
let origin, originalMic, originalGroups, originalFx, originalMuteAll, micTouched = false, groupsTouched = false, fxTouched = false, muteAllTouched = false;
const sameSession = () => snapshot.instanceId === origin.instanceId && snapshot.sessionId === origin.sessionId;
async function restoreCommand(command, args) {
  assert.ok(sameSession(), "Session changed; restoration cannot edit another session");
  const id = String(++sequence);
  observer.write(JSON.stringify({ v: 1, type: "command", id, ...origin, ifRevision: snapshot.revision, command, args }) + "\n");
  const response = await waitObserver(() => lines.find(m => m.id === id && (m.type === "ack" || m.type === "error")));
  if (response.type === "error") throw new Error("Restoration rejected: " + response.code);
  await waitObserver(() => sameSession() && snapshot.revision >= response.revision);
}
async function restoreGroups() {
  for (const saved of originalGroups) {
    assert.ok(sameSession(), "Session changed; restoration cannot edit another session");
    const current = groupTargets().find(g => g.channelId === saved.channelId);
    assert.ok(current, "Original plugin group disappeared during the check");
    if (current.off !== saved.off) await restoreCommand("setPluginGroupOff", { channelId: saved.channelId, index: 1, off: saved.off });
  }
}
async function groupVisual(targets) {
  const total = targets.length, onCount = targets.filter(g => !g.off).length;
  const lamp = onCount === total ? "plugin-all-on" : onCount ? "plugin-all-mixed" : "plugin-all-off";
  const expected = actionSvg(lamp, locales.ko, onCount, total, !snapshot.state.audio.running);
  await host.wait(() => {
    const v = host.visual("group-real");
    return v.state === (onCount === total ? 1 : 0) && svg(v.image) === expected && v.title?.replace(/\n/g, "") === "전체 마이크 · 그룹 1";
  }, 8000);
}
async function fxVisual(on) {
  await host.wait(() => {
    const fx = snapshot.state.fx.find(f => f.id === originalFx.fxId);
    if (!fx || fx.on !== on) return false;
    const lamp = fxMuted(snapshot.state, fx) ? (on ? "muted-on" : "muted-off") : on ? "on" : "off";
    const v = host.visual("fx-real");
    return v.state === (on ? 1 : 0) && svg(v.image) === micSvg(lamp, locales.ko, !snapshot.state.audio.running, false, 144, "fx");
  }, 8000);
}

async function muteAllVisual(muted, channelId) {
  await host.wait(() => {
    const mic = snapshot.state.channels.find(c => c.id === channelId);
    if (!mic || snapshot.state.muteAll.mic !== muted) return false;
    const lamp = micMuted(snapshot.state, mic) ? (mic.on ? "muted-on" : "muted-off") : mic.on ? "on" : "off";
    const all = host.visual("mute-all-real"), channel = host.visual("mute-all-mic-real"), stopped = !snapshot.state.audio.running;
    return all.state === (muted ? 1 : 0) && svg(all.image) === actionSvg(muted ? "mute-every-muted" : "mute-every-clear", locales.ko, snapshot.state.channels.length, 0, stopped)
      && channel.state === (mic.on ? 1 : 0) && svg(channel.image) === micSvg(lamp, locales.ko, stopped);
  }, 8000);
}

try {
  await waitObserver(() => !observer.connecting);
  observer.write(JSON.stringify({ v: 1, type: "hello", id: "1", supportedVersions: [1], token: discovery.token, client: { name: "real-check-observer", version: "1.4.0" } }) + "\n");
  await waitObserver(() => snapshot);
  origin = { instanceId: snapshot.instanceId, sessionId: snapshot.sessionId };
  const mic = snapshot.state.channels[0]; assert.ok(mic, "No mic channel in the current session");
  originalMic = { channelId: mic.id, on: mic.on };
  await host.start(appdata, "ko");
  host.appear("key-real", { settingsVersion: 1, channelId: mic.id, channelName: mic.name, mode: "toggle", nameFallback: false });
  await host.wait(() => {
    const muted = micMuted(snapshot.state, mic);
    const lamp = muted ? mic.on ? "muted-on" : "muted-off" : mic.on ? "on" : "off";
    return host.visual("key-real").state === (mic.on ? 1 : 0) && svg(host.visual("key-real").image) === micSvg(lamp, locales.ko, !snapshot.state.audio.running);
  }, 8000);
  micTouched = true;
  for (const on of [!originalMic.on, originalMic.on]) {
    const revision = snapshot.revision;
    host.keyDown("key-real"); host.keyUp("key-real");
    await waitObserver(() => sameSession() && snapshot.revision > revision && snapshot.state.channels.find(c => c.id === originalMic.channelId)?.on === on);
    await host.wait(() => host.visual("key-real").state === (on ? 1 : 0), 8000);
  }
  micTouched = false;
  console.log("MIC_OK (toggled and restored)");

  originalGroups = groupTargets();
  if (!originalGroups.length) console.log("GROUP_ALL_SKIPPED (no group 1 in the current session)");
  else {
    const hello = lines.find(m => m.type === "helloAck");
    assert.ok(hello?.capabilities.includes("pluginGroupsEverywhere"), "Group-all check requires LiveMix 0.10.0 or later");
    host.appear("group-real", { settingsVersion: 1, groupIndex: 1, mode: "toggle", shortTitle: "" }, 1, "plugin-group-all");
    await groupVisual(originalGroups);
    const firstOff = originalGroups.some(g => !g.off);
    groupsTouched = true;
    for (const off of [firstOff, !firstOff]) {
      const revision = snapshot.revision;
      host.keyDown("group-real"); host.keyUp("group-real");
      await waitObserver(() => sameSession() && snapshot.revision > revision && groupTargets().length === originalGroups.length && groupTargets().every(g => g.off === off));
      await groupVisual(groupTargets());
    }
    // A mixed initial state cannot be restored by two global toggles. Restore only differing slots.
    await restoreGroups(); await groupVisual(originalGroups); groupsTouched = false;
    console.log("GROUP_ALL_OK");
  }
  const hello = lines.find(m => m.type === "helloAck"), fx = snapshot.state.fx[0];
  if (!hello?.capabilities.includes("fxOn")) console.log("FX_ON_SKIPPED (LiveMix does not advertise fxOn)");
  else if (!fx) console.log("FX_ON_SKIPPED (no FX in the current session)");
  else {
    originalFx = { fxId: fx.id, on: fx.on };
    host.appear("fx-real", { settingsVersion: 1, fxId: fx.id, fxName: fx.name, mode: "toggle", nameFallback: false }, 2, "fx");
    await fxVisual(originalFx.on);
    fxTouched = true;
    for (const on of [!originalFx.on, originalFx.on]) {
      const revision = snapshot.revision;
      host.keyDown("fx-real"); host.keyUp("fx-real");
      await waitObserver(() => sameSession() && snapshot.revision > revision && snapshot.state.fx.find(f => f.id === originalFx.fxId)?.on === on);
      await fxVisual(on);
    }
    fxTouched = false;
    console.log("FX_ON_OK");
  }
  const muteMic = snapshot.state.channels[0];
  if (!hello?.capabilities.includes("muteAll")) console.log("MUTE_ALL_SKIPPED (LiveMix does not advertise muteAll)");
  else if (!muteMic) console.log("MUTE_ALL_SKIPPED (no mic in the current session)");
  else {
    originalMuteAll = snapshot.state.muteAll.mic;
    host.appear("mute-all-real", { mode: "toggle" }, 3, "mic-mute-all");
    host.appear("mute-all-mic-real", { channelId: muteMic.id, channelName: muteMic.name, mode: "toggle", nameFallback: false }, 4);
    await muteAllVisual(originalMuteAll, muteMic.id);
    muteAllTouched = true;
    for (const muted of [!originalMuteAll, originalMuteAll]) {
      const revision = snapshot.revision;
      host.keyDown("mute-all-real"); host.keyUp("mute-all-real");
      await waitObserver(() => sameSession() && snapshot.revision > revision && snapshot.state.muteAll.mic === muted);
      await muteAllVisual(muted, muteMic.id);
    }
    muteAllTouched = false;
    console.log("MUTE_ALL_OK");
  }
  assert.equal(host.output.includes(discovery.token), false, "Token must not appear in plugin logs");
  console.log("REAL_CHECK_OK");
} finally {
  try {
    if (muteAllTouched) await restoreCommand("setMuteAll", { group: "mic", muted: originalMuteAll });
    if (fxTouched) await restoreCommand("setFxOn", originalFx);
    if (groupsTouched) await restoreGroups();
    if (micTouched) await restoreCommand("setChannelOn", originalMic);
  } finally {
    try { await host.close(); } finally { observer.destroy(); }
  }
}
