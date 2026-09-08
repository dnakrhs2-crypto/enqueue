import assert from "node:assert/strict";
import type { TestContext } from "node:test";
import { FakeHost } from "./fake-host.mjs";
import { FakeLiveMix } from "./fake-livemix-server.mjs";
import { until } from "./helpers.mjs";
import type { Projection } from "../src/livemix/protocol.js";

export async function setupActions(t: TestContext, language = "ko") {
  const server = await new FakeLiveMix().start(), host = new FakeHost();
  t.after(async () => { try { await host.close(); assert.ok(!host.output.includes(server.token)); } finally { await server.close(); } });
  const state = server.snapshot.state as Projection, channel = state.channels[0]!, fx = state.fx[0]!;
  state.audio.running = true;
  channel.pluginGroups = [{ index: 1, off: false }, { index: 2, off: true }]; channel.muteGroup = true;
  channel.sends[0]!.amount = 0.35; fx.muteGroup = true;
  state.channels.push({ ...structuredClone(channel), id: "33333333333343338333333333333333", name: "게스트", muteGroup: false },
    { ...structuredClone(channel), id: "55555555555545558555555555555555", name: "예비", on: false });
  state.fx.push({ ...fx, id: "44444444444444448444444444444444", name: "딜레이", muteGroup: false });
  const settings = { settingsVersion: 1, channelId: channel.id, channelName: channel.name, fxId: fx.id, fxName: fx.name,
    groupIndex: 1, mode: "toggle", nameFallback: true, shortTitle: "", stepPercent: 1, pressMode: "pre-post", display: "connection" };
  await host.start(server.appdata, language);
  return { server, host, settings };
}
export const svg = (url?: string): string => url?.startsWith("data:image/svg+xml;base64,") ? Buffer.from(url.split(",")[1]!, "base64").toString("utf8") : "";
export const title = (host: FakeHost, id: string): string => (host.visual(id).title ?? "").replace(/\n/g, "");
export const commands = (server: FakeLiveMix, name?: string): any[] => server.received.filter((m: any) => m.type === "command" && (name ? m.command === name : m.command !== "requestState"));
export const alerts = (host: FakeHost, id: string): any[] => host.records.filter(r => r.context === id && r.event === "showAlert");
export async function key(host: FakeHost, id: string, state: number, color: string, label = ""): Promise<void> {
  await host.wait(() => { const v = host.visual(id); return v.state === state && svg(v.image).includes(color) && svg(v.image).includes(label)
    && !!v.title && !/LiveMix|연결 확인|Checking/.test(v.title); });
}
export async function options(host: FakeHost, id: string) {
  const inspector = await host.openInspector(id); inspector.request();
  await until(host, "pi", () => inspector.pi.messages.some(m => m.payload.op === "options"));
  return inspector;
}
export async function barrier(host: FakeHost, id: string): Promise<void> { await options(host, id); await host.closeInspector(id); }
