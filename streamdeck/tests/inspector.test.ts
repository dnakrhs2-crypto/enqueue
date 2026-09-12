import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { resolve } from "node:path";
import { runInNewContext } from "node:vm";
import { pluginRoot } from "./helpers.mjs";

// Minimal DOM surface for executing the shipped PI; text setters are safe sinks.
class Element {
  value = ""; checked = false; disabled = false; hidden = false; lang = ""; textContent = "";
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
    const ids = [...["channel", "fx", "group", "mode", "step", "target", "press", "display", "fallback", "title"].flatMap(id => [id, `${id}-row`, `${id}-label`]), "short-title", "status", "note"];
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

test("all nine shipped PI views expose only relevant fields, localized modes/notes and instant offline-safe settings", async () => {
  const [strings, script] = await Promise.all(["strings.js", "inspector.js"].map(file => readFile(resolve(pluginRoot, "ui", file), "utf8")));
  for (const language of ["ko", "en"]) for (const kind of ["mic", "all-mics", "mic-mute-group", "fx-mute-group", "plugin-group", "plugin-group-all", "fx-send", "fx-send-step", "status"]) {
    const fields = ["channel", "fx", "group", "mode", "step", "target", "press", "display", "fallback", "title"];
    const elements = Object.fromEntries([...fields.flatMap(id => [id, `${id}-row`, `${id}-label`]), "short-title", "status", "note"].map(id => [id, new Element()]));
    const document = { documentElement: new Element(), getElementById: (id: string) => elements[id], createElement: () => new Element() };
    const window: Record<string, any> = { addEventListener() {} }, environment = { window, document, WebSocket: Socket };
    runInNewContext(strings!, environment); runInNewContext(script!, environment);
    const settings = { channelId: "channel", channelName: "진행 <&>", fxId: "fx", fxName: "리버브 <&>", groupIndex: 2, nameFallback: true };
    window.connectElgatoStreamDeckSocket(1234, "pi", "registerPropertyInspector", JSON.stringify({ application: { language } }), JSON.stringify({ context: "key", action: `com.gomtwigim.livemix.${kind}`, payload: { settings } }));
    const socket = Socket.current; socket.onopen(); const locale = window.LiveMixStrings[language];
    const expected: Record<string, string[]> = {
      mic: ["channel", "mode", "fallback", "title"], "all-mics": ["mode", "title"],
      "mic-mute-group": ["mode", "title"], "fx-mute-group": ["mode", "title"],
      "plugin-group": ["channel", "group", "mode", "fallback", "title"],
      "plugin-group-all": ["group", "mode", "title"],
      "fx-send": ["channel", "fx", "step", "press", "fallback"],
      "fx-send-step": ["channel", "fx", "mode", "step", "fallback"], status: ["display"] };
    assert.deepEqual(fields.filter(id => !elements[`${id}-row`]!.hidden), expected[kind], kind);
    for (const id of fields) assert.ok(elements[`${id}-label`]!.textContent);
    if (kind === "all-mics") assert.deepEqual(elements.mode!.children.map(c => c.textContent), [locale.toggle, locale.setAllOn, locale.setAllOff]);
    if (kind === "plugin-group-all") assert.deepEqual(elements.mode!.children.map(c => c.textContent), [locale.toggle, locale.on, locale.off]);
    if (kind.endsWith("mute-group")) assert.deepEqual(elements.mode!.children.map(c => c.textContent), [locale.toggle, locale.mute, locale.unmute]);
    if (kind === "fx-send") assert.deepEqual(elements.step!.children.map(c => c.value), ["1", "5"]);
    if (kind === "fx-send-step") {
      assert.deepEqual(elements.mode!.children.map(c => c.value), ["up", "down", "set"]);
      assert.deepEqual(elements.mode!.children.map(c => c.textContent), language === "ko" ? ["올리기", "내리기", "값으로 설정"] : ["Increase", "Decrease", "Set value"]);
      assert.deepEqual(elements.step!.children.map(c => c.textContent), ["1%", "5%", "10%"]);
      assert.equal(elements.mode!.value, "up"); assert.equal(elements.step!.value, "5"); assert.equal(elements.target!.value, "50");
    }
    const live = { op: "options", context: "key", requestId: "pi1", sequence: 1, connection: "ready", instanceId: "i", sessionId: "s", revision: 2,
      message: locale.connected, channels: [{ id: "channel", name: settings.channelName, groupIndices: [1, 2] }],
      fx: [{ id: "fx", name: settings.fxName }, { id: "fx2", name: settings.fxName }], groupIndices: [1, 2], muteGroupCounts: { mic: 2, fx: 1 } };
    socket.receive({ event: "sendToPropertyInspector", context: "pi", payload: live });
    assert.equal(elements.fx!.children[0]!.textContent, settings.fxName + " · fx");
    assert.deepEqual(elements.group!.children.map(c => c.value), ["1", "2"]);
    if (kind.endsWith("mute-group")) assert.equal(elements.note!.textContent, locale.membershipNote + "\n" + locale.targets.replace("{count}", kind.startsWith("mic") ? "2" : "1"));
    if (kind === "plugin-group") assert.equal(elements.note!.textContent, locale.groupNote);
    if (kind === "plugin-group-all") assert.equal(elements.note!.textContent, locale.groupAllNote + "\n" + locale.groupNote);
    if (kind === "fx-send") assert.equal(elements.note!.textContent, locale.dialNote);
    socket.receive({ event: "sendToPropertyInspector", context: "wrong", payload: { ...live, sequence: 2, message: "WRONG" } });
    socket.receive({ event: "sendToPropertyInspector", context: "pi", payload: { ...live, revision: 1, sequence: 2, message: "STALE" } });
    assert.equal(elements.status!.textContent, locale.connected);
    socket.receive({ event: "sendToPropertyInspector", context: "pi", payload: { ...live, sequence: 2, channels: [{ ...live.channels[0], groupIndices: [1] }], groupIndices: [1] } });
    assert.ok(elements.group!.children[0]!.textContent.includes(locale.missingGroup)); assert.equal(elements.group!.value, "2");
    socket.receive({ event: "sendToPropertyInspector", context: "pi", payload: { ...live, sequence: 3, connection: "disabled", message: locale.disabledHelp } });
    for (const id of ["channel", "fx", "group"]) assert.equal(elements[id]!.disabled, true);
    assert.equal(elements.fx!.value, settings.fxId); assert.equal(elements.channel!.value, settings.channelId);
    const control = kind === "fx-send" ? "step" : kind === "status" ? "display" : "mode";
    elements[control]!.value = control === "step" ? "5" : control === "display" ? "audio" : kind === "fx-send-step" ? "set" : kind.endsWith("mute-group") ? "mute" : "on";
    elements[control]!.listeners.get("change")!();
    const saved = socket.sent.at(-1)!;
    assert.equal(saved.event, "setSettings"); assert.equal(saved.payload.channelId, settings.channelId); assert.equal(saved.payload.fxId, settings.fxId);
    assert.equal(saved.payload[control === "step" ? "stepPercent" : control], control === "step" ? 5 : elements[control]!.value);
    const count = socket.sent.length; socket.receive({ event: "didReceiveSettings", context: "pi", payload: { settings: saved.payload } }); assert.equal(socket.sent.length, count);
    if (kind === "fx-send") {
      elements.press!.value = "none"; elements.press!.listeners.get("change")!(); assert.equal(socket.sent.at(-1)!.payload.pressMode, "none");
    }
    if (kind === "fx-send-step") {
      assert.equal(elements["target-row"]!.hidden, false); assert.equal(elements["step-row"]!.hidden, true);
      for (const [value, expected] of [["72", 72], ["-10", 0], ["110", 100]] as const) {
        elements.target!.value = value; elements.target!.listeners.get("change")!();
        assert.equal(socket.sent.at(-1)!.payload.targetPercent, expected); assert.equal(elements.target!.value, String(expected));
      }
      const count = socket.sent.length;
      elements.target!.value = ""; elements.target!.listeners.get("change")!();
      assert.equal(socket.sent.length, count); assert.equal(elements.target!.value, "100");
      elements.mode!.value = "down"; elements.mode!.listeners.get("change")!();
      assert.equal(elements["target-row"]!.hidden, true); assert.equal(elements["step-row"]!.hidden, false);
      elements.step!.value = "10"; elements.step!.listeners.get("change")!(); assert.equal(socket.sent.at(-1)!.payload.stepPercent, 10);
      elements.mode!.value = "set"; elements.mode!.listeners.get("change")!(); assert.equal(elements.target!.value, "100");
    }
    socket.close(); assert.equal(elements.status!.textContent, locale.offlineHelp); assert.equal(elements.fx!.value, settings.fxId);
  }
});

test("plugin-group-all PI unions slots with per-channel counts, saves only its fields and preserves missing/offline selections", async () => {
  const [strings, script] = await Promise.all(["strings.js", "inspector.js"].map(file => readFile(resolve(pluginRoot, "ui", file), "utf8")));
  for (const language of ["ko", "en"]) {
    const fields = ["channel", "fx", "group", "mode", "step", "target", "press", "display", "fallback", "title"];
    const elements = Object.fromEntries([...fields.flatMap(id => [id, `${id}-row`, `${id}-label`]), "short-title", "status", "note"].map(id => [id, new Element()]));
    const document = { documentElement: new Element(), getElementById: (id: string) => elements[id], createElement: () => new Element() };
    const window: Record<string, any> = { addEventListener() {} }, environment = { window, document, WebSocket: Socket };
    runInNewContext(strings!, environment); runInNewContext(script!, environment);
    window.connectElgatoStreamDeckSocket(1234, "pi", "registerPropertyInspector", JSON.stringify({ application: { language } }),
      JSON.stringify({ context: "key", action: "com.gomtwigim.livemix.plugin-group-all", payload: { settings: { groupIndex: 2, mode: "toggle" } } }));
    const socket = Socket.current; socket.onopen(); const locale = window.LiveMixStrings[language];
    assert.equal(elements.group!.disabled, true); assert.equal(elements.group!.value, "2");
    assert.deepEqual(fields.filter(id => !elements[`${id}-row`]!.hidden), ["group", "mode", "title"]);
    const live = { op: "options", context: "key", requestId: "pi1", sequence: 1, connection: "ready", instanceId: "i", sessionId: "s", revision: 1,
      message: locale.connected, channels: [{ id: "a", groupIndices: [5, 1] }, { id: "b", groupIndices: [2, 1] }, { id: "c", groupIndices: [2] }], groupIndices: [4] };
    socket.receive({ event: "sendToPropertyInspector", context: "pi", payload: live });
    assert.equal(elements.group!.disabled, false); assert.equal(elements.group!.value, "2");
    assert.deepEqual(elements.group!.children.map(c => c.value), ["1", "2", "5"]);
    assert.deepEqual(elements.group!.children.map(c => c.textContent), language === "ko"
      ? ["그룹 1 · 마이크 2개", "그룹 2 · 마이크 2개", "그룹 5 · 마이크 1개"] : ["Group 1 · 2 mics", "Group 2 · 2 mics", "Group 5 · 1 mic"]);
    assert.equal(elements.note!.textContent, locale.groupAllNote + "\n" + locale.groupNote); assert.equal(elements.note!.hidden, false);
    elements.group!.value = "5"; elements.group!.listeners.get("change")!();
    assert.deepEqual(socket.sent.at(-1)!.payload, { groupIndex: 5, mode: "toggle", settingsVersion: 1 });
    socket.receive({ event: "sendToPropertyInspector", context: "pi", payload: { ...live, sequence: 2, channels: [{ id: "a", groupIndices: [1] }] } });
    assert.equal(elements.group!.value, "5"); assert.equal(elements.group!.children[0]!.disabled, true);
    assert.equal(elements.group!.children[0]!.textContent, locale.group + " 5 · " + locale.missingGroup);
    socket.receive({ event: "sendToPropertyInspector", context: "pi", payload: { ...live, sequence: 3, connection: "disabled", channels: undefined, message: locale.disabledHelp } });
    assert.equal(elements.group!.disabled, true); assert.equal(elements.group!.value, "5");
    assert.equal(elements.group!.children[0]!.textContent, locale.group + " 5");
    elements.mode!.value = "off"; elements.mode!.listeners.get("change")!();
    elements["short-title"]!.value = "Live <&>"; elements["short-title"]!.listeners.get("input")!();
    assert.deepEqual(socket.sent.at(-1)!.payload, { groupIndex: 5, mode: "off", shortTitle: "Live <&>", settingsVersion: 1 });
    socket.receive({ event: "sendToPropertyInspector", context: "pi", payload: { ...live, sequence: 4 } });
    assert.equal(elements.group!.value, "5"); assert.equal(elements.group!.disabled, false);
    socket.close(); assert.equal(elements.group!.value, "5"); assert.equal(elements.group!.disabled, true);
  }
});
