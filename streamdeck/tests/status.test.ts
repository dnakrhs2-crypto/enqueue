import test from "node:test";
import assert from "node:assert/strict";
import { setTimeout as delay } from "node:timers/promises";
import type { Snapshot } from "../src/livemix/protocol.js";
import { until } from "./helpers.mjs";
import { setupActions, title, key, commands, alerts, barrier } from "./action-helpers.js";

test("status selects connection/session/audio; session dirty marker and audio follow external state", async t => {
  const { server, host, settings } = await setupActions(t);
  host.appear("connection", settings, 0, "status"); host.appear("session", { ...settings, display: "session" }, 1, "status");
  host.appear("audio", { ...settings, display: "audio" }, 2, "status");
  await key(host, "connection", 1, "#35D07F"); assert.equal(title(host, "connection"), "연결됨");
  await host.wait(() => title(host, "session").includes("Control test") && title(host, "session").endsWith(" *"));
  await host.wait(() => title(host, "audio") === "오디오 running");
  server.mutate((s: Snapshot) => { s.state.session.name = "방송"; s.state.session.dirty = false; s.state.audio.running = false; });
  await host.wait(() => title(host, "session") === "방송" && title(host, "audio") === "오디오 멈춤");
  assert.equal(host.visual("audio").state, 0);
  server.mutate((s: Snapshot) => { s.state.session.dirty = true; }); await host.wait(() => title(host, "session") === "방송 *");
  assert.equal(commands(server).length, 0);
});

test("status keyDown requests state at most once per 2 seconds per key, including reappearance; keyUp inert", async t => {
  const { server, host, settings } = await setupActions(t);
  host.appear("a", settings, 0, "status"); host.appear("b", settings, 1, "status"); await key(host, "a", 1, "#35D07F");
  for (let n = 0; n < 5; n++) { host.keyDown("a"); host.keyUp("a"); }
  host.keyDown("b"); await until(server, "message", () => commands(server, "requestState").length === 2);
  await barrier(host, "a"); assert.equal(commands(server, "requestState").length, 2);
  host.disappear("a"); host.appear("a", settings, 0, "status"); host.keyDown("a"); await barrier(host, "a");
  assert.equal(commands(server, "requestState").length, 2);
  await delay(2050); host.keyDown("a"); host.keyUp("a"); await until(server, "message", () => commands(server, "requestState").length === 3);
  assert.equal(commands(server).length, 0); assert.equal(alerts(host, "a").length, 0);
});

test("status shows disabled/offline and alerts on explicit refresh without issuing edits", async t => {
  const { server, host, settings } = await setupActions(t, "en");
  host.appear("a", { ...settings, display: "session" }, 0, "status"); await key(host, "a", 1, "#35D07F");
  await server.setStatus("disabled"); await host.wait(() => host.visual("a").title?.replace(/\n/g, " ") === "Control disabled");
  host.keyDown("a"); host.keyUp("a"); await host.wait(() => alerts(host, "a").length === 1);
  assert.equal(commands(server, "requestState").length, 0); assert.equal(commands(server).length, 0);
});
