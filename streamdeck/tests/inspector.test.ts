import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { resolve } from "node:path";
import { runInNewContext } from "node:vm";
import { pluginRoot } from "./helpers.mjs";

// Minimal DOM surface for executing the shipped PI; text setters are safe sinks.
class Element {
  value = ""; checked = false; disabled = false; lang = ""; textContent = "";
  children: Element[] = []; listeners = new Map<string, () => void>();
  get options(): Element[] { return this.children; }
  set innerHTML(_: string) { throw new Error("Names must never use innerHTML"); }
  addEventListener(name: string, handler: () => void): void { this.listeners.set(name, handler); }
  append(child: Element): void { this.children.push(child); }
  replaceChildren(): void { this.children = []; }
}
class Socket {
  static OPEN = 1;
  static current: Socket;
  readyState = 1; sent: Record<string, any>[] = [];
  onopen = (): void => {}; onmessage = (_: { data: string }): void => {}; onclose = (): void => {};
  constructor(readonly url: string) { Socket.current = this; }
  send(value: string): void { this.sent.push(JSON.parse(value)); }
  close(): void { this.onclose(); }
  receive(value: unknown): void { this.onmessage({ data: JSON.stringify(value) }); }
}
test("shipped PI script: text-only names, offline preservation, auto-save, stale-response rejection and ko/en", async () => {
  const [strings, script, html] = await Promise.all(["strings.js", "inspector.js", "inspector.html"].map(file => readFile(resolve(pluginRoot, "ui", file), "utf8")));
  assert.ok(!html!.includes("https://"));
  for (const language of ["ko", "en", "fr"]) {
    const ids = ["channel", "mode", "fallback", "short-title", "status", "channel-label", "mode-label", "fallback-label", "title-label"];
    const elements = Object.fromEntries(ids.map(id => [id, new Element()])); elements.mode!.append(new Element());
    const document = { documentElement: new Element(), getElementById: (id: string) => elements[id], createElement: () => new Element() };
    const window: Record<string, any> = { addEventListener() {} };
    const environment = { window, document, WebSocket: Socket };
    runInNewContext(strings!, environment); runInNewContext(script!, environment);
    const channelId = "11111111111141118111111111111111", name = '<script>alert("곰")</script> & <img>';
    window.connectElgatoStreamDeckSocket(1234, "pi-key-a", "registerPropertyInspector", JSON.stringify({ application: { language } }), JSON.stringify({ context: "key-a", action: "com.gomtwigim.livemix.mic", payload: { settings: { channelId, channelName: name, mode: "off" } } }));
    const socket = Socket.current; socket.onopen();
    assert.equal(elements.channel!.disabled, true); assert.equal(elements.channel!.children[0]?.textContent, name);
    assert.equal(document.documentElement.lang, language === "ko" ? "ko" : "en");
    const reply = { op: "options", context: "key-a", requestId: "pi1", sequence: 1, connection: "ready", instanceId: "a", sessionId: "b", revision: 1, message: "Connected", channels: [{ id: channelId, name }, { id: "second", name }] };
    socket.receive({ event: "sendToPropertyInspector", context: "pi-key-a", payload: reply });
    assert.equal(elements.channel!.disabled, false); assert.equal(elements.channel!.value, channelId);
    assert.equal(elements.channel!.children[0]?.textContent, name + " · " + channelId.slice(0, 8));
    elements.mode!.value = "on"; elements.mode!.listeners.get("change")!();
    const saved = socket.sent.at(-1)!; assert.equal(saved.event, "setSettings"); assert.equal(saved.payload.settingsVersion, 1); assert.equal(saved.payload.channelId, channelId);
    const count = socket.sent.length;
    socket.receive({ event: "didReceiveSettings", context: "pi-key-a", payload: { settings: saved.payload } }); assert.equal(socket.sent.length, count);
    socket.receive({ event: "sendToPropertyInspector", context: "pi-key-a", payload: { ...reply, sequence: 0, message: "STALE" } });
    assert.equal(elements.status!.textContent, "Connected");
    socket.receive({ event: "sendToPropertyInspector", context: "pi-key-a", payload: { ...reply, sequence: 2, connection: "disabled", channels: undefined, message: "Disabled" } });
    assert.equal(elements.channel!.disabled, true); assert.equal(elements.channel!.value, channelId); assert.equal(elements.mode!.value, "on");
    elements.mode!.value = "off"; elements.mode!.listeners.get("change")!(); assert.equal(socket.sent.at(-1)!.payload.mode, "off");
  }
});
