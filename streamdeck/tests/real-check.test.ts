import test from "node:test";
import assert from "node:assert/strict";
import { execFile } from "node:child_process";
import { promisify } from "node:util";
import { FakeLiveMix } from "./fake-livemix-server.mjs";
import { packageRoot } from "./helpers.mjs";
import type { Projection } from "../src/livemix/protocol.js";

// Exercise the real-app checker against isolated fake discovery; never use the user's LiveMix.
for (const scenario of ["on", "off", "mixed", "absent"]) test(`real-app checker: ${scenario} group session restores original switches`, async t => {
  const server = await new FakeLiveMix().start(); t.after(() => server.close());
  const state = server.snapshot.state as Projection, channel = state.channels[0]!;
  channel.pluginGroups = scenario === "absent" ? [] : [{ index: 1, off: scenario === "off" }, { index: 2, off: true }];
  state.channels.push({ ...structuredClone(channel), id: "33333333333343338333333333333333", name: "게스트" },
    { ...structuredClone(channel), id: "55555555555545558555555555555555", name: "예비", pluginGroups: [] });
  if (scenario === "mixed") state.channels[1]!.pluginGroups[0]!.off = true;
  state.fx[0]!.on = scenario !== "off";
  state.fx[0]!.muteGroup = true; state.muteGroups.fx = scenario === "mixed";
  const before = structuredClone(state.channels), beforeFx = structuredClone(state.fx);
  const { stdout, stderr } = await promisify(execFile)(process.execPath, ["tools/real_livemix_check.mjs"], {
    cwd: packageRoot, env: { ...process.env, APPDATA: server.appdata }, windowsHide: true, timeout: 30000
  });
  assert.ok(stdout.includes("MIC_OK") && stdout.includes("REAL_CHECK_OK"));
  assert.ok(stdout.includes(scenario === "absent" ? "GROUP_ALL_SKIPPED (no group 1 in the current session)" : "GROUP_ALL_OK"));
  assert.ok(stdout.includes("FX_ON_OK"));
  assert.equal((stdout + stderr).includes(server.token), false);
  assert.deepEqual(state.channels, before);
  assert.deepEqual(state.fx, beforeFx);
  assert.deepEqual(server.received.filter((m: any) => m.command === "toggleFx").map((m: any) => m.args), [{ fxId: state.fx[0]!.id }, { fxId: state.fx[0]!.id }]);
  const commands = server.received.filter((m: any) => m.command === "setPluginGroupOffEverywhere");
  assert.deepEqual(commands.map((m: any) => m.args.off), scenario === "absent" ? [] : scenario === "off" ? [false, true] : [true, false]);
  assert.equal(server.received.filter((m: any) => m.command === "setPluginGroupOff").length, scenario === "mixed" ? 1 : 0);
});

for (const scenario of ["legacy", "no-fx"] as const) test(`real-app checker: ${scenario} skips FX without sending FX commands`, async t => {
  const server = await new FakeLiveMix({ legacyFx: scenario === "legacy" }).start(); t.after(() => server.close());
  if (scenario === "no-fx") {
    server.snapshot.state.fx = [];
    for (const channel of server.snapshot.state.channels) channel.sends = [];
  }
  const { stdout, stderr } = await promisify(execFile)(process.execPath, ["tools/real_livemix_check.mjs"], {
    cwd: packageRoot, env: { ...process.env, APPDATA: server.appdata }, windowsHide: true, timeout: 30000
  });
  assert.ok(stdout.includes(scenario === "legacy" ? "FX_ON_SKIPPED (LiveMix does not advertise fxOn)" : "FX_ON_SKIPPED (no FX in the current session)"));
  assert.ok(stdout.includes("REAL_CHECK_OK")); assert.equal((stdout + stderr).includes(server.token), false);
  assert.equal(server.received.filter((m: any) => m.command === "toggleFx" || m.command === "setFxOn").length, 0);
});
