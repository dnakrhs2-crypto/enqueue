// Run the built plugin under the fake Stream Deck host against REAL LiveMix (External Control ON).
// An independent TCP observer checks mic and all-mics group toggles; original values are restored.
// Usage from streamdeck/: node tools/real_livemix_check.mjs
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { resolve } from "node:path";
import net from "node:net";
import { FakeHost } from "../tests/fake-host.mjs";
import { actionSvg, micSvg } from "../src/ui/artwork.ts";
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
    if (m.type === "state") snapshot = m;
    else if (m.type === "stateDelta") {
      if (!snapshot || m.instanceId !== snapshot.instanceId || m.sessionId !== snapshot.sessionId || m.baseRevision !== snapshot.revision) {
        observerIssue = new Error("Observer state needs resynchronization"); continue;
      }
      for (const key of ["session", "audio", "muteGroups"]) if (m.changes[key]) Object.assign(snapshot.state[key], m.changes[key]);
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
let origin, originalMic, originalGroups, micTouched = false, groupsTouched = false;
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

try {
  await waitObserver(() => !observer.connecting);
  observer.write(JSON.stringify({ v: 1, type: "hello", id: "1", supportedVersions: [1], token: discovery.token, client: { name: "real-check-observer", version: "1.1.0" } }) + "\n");
  await waitObserver(() => snapshot);
  origin = { instanceId: snapshot.instanceId, sessionId: snapshot.sessionId };
  const mic = snapshot.state.channels[0]; assert.ok(mic, "No mic channel in the current session");
  originalMic = { channelId: mic.id, on: mic.on };
  await host.start(appdata, "ko");
  host.appear("key-real", { settingsVersion: 1, channelId: mic.id, channelName: mic.name, mode: "toggle", nameFallback: false });
  await host.wait(() => {
    const muted = snapshot.state.muteGroups.mic && mic.muteGroup;
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
  assert.equal(host.output.includes(discovery.token), false, "Token must not appear in plugin logs");
  console.log("REAL_CHECK_OK");
} finally {
  try {
    if (groupsTouched) await restoreGroups();
    if (micTouched) await restoreCommand("setChannelOn", originalMic);
  } finally {
    try { await host.close(); } finally { observer.destroy(); }
  }
}
