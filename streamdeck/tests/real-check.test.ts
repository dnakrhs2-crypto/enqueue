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
  const before = structuredClone(state.channels);
  const { stdout, stderr } = await promisify(execFile)(process.execPath, ["tools/real_livemix_check.mjs"], {
    cwd: packageRoot, env: { ...process.env, APPDATA: server.appdata }, windowsHide: true, timeout: 30000
  });
  assert.ok(stdout.includes("MIC_OK") && stdout.includes("REAL_CHECK_OK"));
  assert.ok(stdout.includes(scenario === "absent" ? "GROUP_ALL_SKIPPED (no group 1 in the current session)" : "GROUP_ALL_OK"));
  assert.equal((stdout + stderr).includes(server.token), false);
  assert.deepEqual(state.channels, before);
  const commands = server.received.filter((m: any) => m.command === "setPluginGroupOffEverywhere");
  assert.deepEqual(commands.map((m: any) => m.args.off), scenario === "absent" ? [] : scenario === "off" ? [false, true] : [true, false]);
  assert.equal(server.received.filter((m: any) => m.command === "setPluginGroupOff").length, scenario === "mixed" ? 1 : 0);
});
