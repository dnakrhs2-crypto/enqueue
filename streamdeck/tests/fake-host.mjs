import assert from "node:assert/strict";
import { EventEmitter, once } from "node:events";
import { spawn } from "node:child_process";
import { resolve } from "node:path";
import { performance } from "node:perf_hooks";
import { WebSocket, WebSocketServer } from "ws";
import { DeviceType } from "@elgato/streamdeck";
import { pluginRoot, until } from "./helpers.mjs";

export const actionUUID = "com.gomtwigim.livemix.mic";
export class FakeHost extends EventEmitter {
  contexts = new Map();
  records = [];
  errors = [];
  output = "";
  registration = undefined;
  runtimeUUID = "test-runtime-1";
  inspectors = new Map();
  fenceSequence = 0;
  info = {
    application: { font: "Segoe UI", language: "ko", platform: "windows", platformVersion: "10", version: "7.1.0" },
    colors: { buttonMouseOverBackgroundColor: "#464646", buttonPressedBackgroundColor: "#202020", buttonPressedBorderColor: "#969696", buttonPressedTextColor: "#ffffff", highlightColor: "#4c8dff" },
    devicePixelRatio: 2,
    devices: [{ id: "fake-mobile", name: "Fake Mobile", size: { columns: 3, rows: 2 }, type: DeviceType.StreamDeckMobile },
      { id: "fake-plus", name: "Fake Stream Deck +", size: { columns: 4, rows: 2 }, type: DeviceType.StreamDeckPlus }],
    plugin: { uuid: "com.gomtwigim.livemix", version: "1.1.0.0" }
  };
  async start(appdata, language = "ko") {
    this.info.application.language = language;
    assert.equal(this.info.devices[0].type, 3, "Use the installed SDK's Mobile enum");
    assert.equal(this.info.devices[1].type, 7, "Use the installed SDK's Stream Deck + enum");
    this.server = new WebSocketServer({ host: "127.0.0.1", port: 0 }); await once(this.server, "listening");
    this.server.on("connection", socket => {
      let registered = false;
      socket.on("error", error => { if (!this.closing) { this.errors.push(error); this.emit("record"); } });
      socket.on("message", raw => {
        try {
          const m = JSON.parse(raw.toString());
          if (!registered) {
            registered = true;
            if (m.event === "registerPlugin") {
              assert.deepEqual(m, { event: "registerPlugin", uuid: this.runtimeUUID });
              assert.equal(this.registration, undefined, "Exactly one SDK connection/registration");
              this.registration = m; this.pluginSocket = socket;
            } else {
              assert.equal(m.event, "registerPropertyInspector");
              const pi = this.inspectors.get(m.uuid); assert.ok(pi); pi.socket = socket;
              this.event("propertyInspectorDidAppear", pi.context);
            }
            this.emit("record"); return;
          }
          if (socket === this.pluginSocket) this.receivePlugin(m);
          else this.receiveInspector(socket, m);
        } catch (error) { this.errors.push(error); this.emit("record"); }
      });
    });
    this.child = spawn(process.execPath, [resolve(pluginRoot, "bin/plugin.js"), "-port", String(this.server.address().port), "-pluginUUID", this.runtimeUUID, "-registerEvent", "registerPlugin", "-info", JSON.stringify(this.info)], {
      cwd: pluginRoot, env: { ...process.env, APPDATA: appdata }, shell: false, windowsHide: true, stdio: ["ignore", "pipe", "pipe"]
    });
    this.child.stdout.on("data", bytes => { this.output += bytes; });
    this.child.stderr.on("data", bytes => { this.output += bytes; });
    this.child.on("error", error => { this.errors.push(error); this.emit("record"); });
    this.child.on("exit", (code, signal) => { this.exit = { code, signal }; this.emit("record"); });
    await this.wait(() => !!this.registration, 5000);
    for (const device of this.info.devices) this.send({ event: "deviceDidConnect", device: device.id, deviceInfo: device });
    return this;
  }
  send(message) { this.pluginSocket.send(JSON.stringify(message)); }
  event(event, context, payload = {}, extra = {}) {
    const c = this.contexts.get(context); assert.ok(c, `Known context ${context}`);
    this.send({ event, action: c.action, context, device: c.device, payload: { controller: c.controller, coordinates: c.coordinates, isInMultiAction: false, resources: {}, settings: c.settings, state: c.state, ...payload }, ...extra });
  }
  appear(context, settings, column = 0, action = "mic", controller = "Keypad") {
    this.contexts.set(context, { device: controller === "Encoder" ? "fake-plus" : "fake-mobile", action: `com.gomtwigim.livemix.${action}`, controller,
      coordinates: { column, row: 0 }, settings, state: 0, visible: true, images: {}, titles: {}, feedback: {} });
    this.event("willAppear", context);
  }
  appearDial(context, settings, column = 0) { this.appear(context, settings, column, "fx-send", "Encoder"); }
  dialRotate(context, ticks, pressed = false) { this.event("dialRotate", context, { ticks, pressed }); }
  dialDown(context) { this.event("dialDown", context); }
  dialUp(context) { this.event("dialUp", context); }
  touch(context, hold = false) { this.event("touchTap", context, { hold, tapPos: [150, 40] }); }
  keyDown(context) { this.event("keyDown", context); }
  keyUp(context) { this.event("keyUp", context); }
  settings(context, settings) { const c = this.contexts.get(context); c.settings = settings; this.event("didReceiveSettings", context); }
  disappear(context) {
    const c = this.contexts.get(context);
    if (c.retiring) return c.retiring;
    this.event("willDisappear", context);
    // Output already on the opposite side of the WebSocket can arrive after our send.
    // A transport fence lets the plugin receive disappearance before enforcing no more output.
    c.retiring = this.fence().then(() => { c.visible = false; });
    return c.retiring;
  }
  fence() {
    const tag = Buffer.from(`fence-${++this.fenceSequence}`);
    return new Promise((resolve, reject) => {
      const finish = error => { clearTimeout(timer); this.pluginSocket.off("pong", pong); error ? reject(error) : resolve(); };
      const pong = data => { if (data.equals(tag)) finish(); };
      const timer = setTimeout(() => finish(new Error("Plugin did not process the WebSocket fence")), 2000);
      this.pluginSocket.on("pong", pong); this.pluginSocket.ping(tag);
    });
  }
  receivePlugin(m) {
    if (["setState", "setImage", "setTitle", "setFeedback", "showAlert"].includes(m.event)) {
      const c = this.contexts.get(m.context); assert.ok(c?.visible, `Output to unknown/disappeared context ${m.context}`);
      this.records.push({ ...m, at: performance.now() });
      if (m.event === "setFeedback") { assert.equal(c.controller, "Encoder"); Object.assign(c.feedback, m.payload); }
      if (m.event === "setState") c.state = m.payload.state;
      if (m.event === "setImage") for (const state of m.payload.state === undefined ? [0, 1] : [m.payload.state]) c.images[state] = m.payload.image;
      if (m.event === "setTitle") for (const state of m.payload.state === undefined ? [0, 1] : [m.payload.state]) c.titles[state] = m.payload.title;
    } else if (m.event === "setSettings" || m.event === "getSettings") {
      const c = this.contexts.get(m.context); assert.ok(c);
      if (m.event === "setSettings") c.settings = m.payload;
      this.event("didReceiveSettings", m.context, {}, m.id ? { id: m.id } : {});
      for (const pi of this.inspectors.values()) if (pi.context === m.context && pi.socket?.readyState === WebSocket.OPEN) pi.socket.send(JSON.stringify({ event: "didReceiveSettings", context: pi.uuid, payload: { settings: c.settings }, ...(m.id ? { id: m.id } : {}) }));
    } else if (m.event === "sendToPropertyInspector") {
      const pi = [...this.inspectors.values()].find(pi => pi.context === m.context && pi.socket?.readyState === WebSocket.OPEN);
      assert.ok(pi, "No replies to a closed/wrong PI");
      pi.socket.send(JSON.stringify({ ...m, context: pi.uuid }));
    } else if (m.event === "getGlobalSettings" || m.event === "setGlobalSettings") {
      if (m.event === "setGlobalSettings") this.globalSettings = m.payload;
      this.send({ event: "didReceiveGlobalSettings", payload: { settings: this.globalSettings ?? {} }, ...(m.id ? { id: m.id } : {}) });
    } else if (m.event !== "logMessage") throw new Error(`Unexpected SDK output ${m.event}`);
    this.emit("record", m);
  }
  receiveInspector(socket, m) {
    const pi = [...this.inspectors.values()].find(pi => pi.socket === socket); assert.ok(pi);
    assert.equal(m.context, pi.uuid);
    if (m.event === "sendToPlugin") this.event("sendToPlugin", pi.context, m.payload);
    else if (m.event === "setSettings") this.settings(pi.context, m.payload);
    else if (m.event === "getSettings") socket.send(JSON.stringify({ event: "didReceiveSettings", context: pi.uuid, payload: { settings: this.contexts.get(pi.context).settings }, ...(m.id ? { id: m.id } : {}) }));
    else throw new Error(`Unexpected PI event ${m.event}`);
  }
  async openInspector(context) {
    /** @type {Array<{event: string, context: string, payload: any}>} */
    const messages = [];
    const uuid = `pi-${context}`, pi = { uuid, context, socket: undefined, messages };
    this.inspectors.set(uuid, pi);
    const client = new WebSocket(`ws://127.0.0.1:${this.server.address().port}`);
    client.on("message", bytes => { pi.messages.push(JSON.parse(bytes.toString())); this.emit("pi"); });
    await once(client, "open"); client.send(JSON.stringify({ event: "registerPropertyInspector", uuid }));
    await this.wait(() => !!pi.socket);
    return { client, pi, request: (requestId = "pi1") => client.send(JSON.stringify({ event: "sendToPlugin", context: uuid, action: this.contexts.get(context).action, payload: { op: "getOptions", requestId } })) };
  }
  async closeInspector(context) {
    const pi = [...this.inspectors.values()].find(p => p.context === context);
    if (!pi) return;
    this.event("propertyInspectorDidDisappear", context);
    await this.fence(); // Drain replies sent before the plugin received the close event.
    pi.socket?.close(); this.inspectors.delete(pi.uuid);
  }
  wait(condition, timeout = 4000) {
    return until(this, "record", () => {
      assert.deepEqual(this.errors, []);
      if (this.exit && !this.closing) throw new Error(`Plugin exited: ${JSON.stringify(this.exit)}\n${this.output}`);
      return condition();
    }, timeout);
  }
  visual(context) {
    const c = this.contexts.get(context);
    return { state: c.state, image: c.images[c.state], title: c.titles[c.state] };
  }
  feedback(context) { return Object.fromEntries(Object.entries(this.contexts.get(context).feedback).map(([key, value]) => [key, typeof value === "object" ? value.value : value])); }
  assertBudget() {
    for (const record of this.records) {
      const window = this.records.filter(r => r.context === record.context && r.at <= record.at && r.at > record.at - 1000);
      assert.ok(window.length <= 10, `10 calls/s budget exceeded: ${window.map(r => r.event).join(", ")}`);
    }
  }
  async close() {
    this.closing = true;
    for (const c of [...this.inspectors.values()]) await this.closeInspector(c.context);
    for (const [id, c] of this.contexts) if (c.visible && this.pluginSocket?.readyState === WebSocket.OPEN) await this.disappear(id);
    if (this.pluginSocket?.readyState === WebSocket.OPEN) for (const device of this.info.devices) this.send({ event: "deviceDidDisconnect", device: device.id });
    if (this.child && !this.exit) {
      this.pluginSocket?.close();
      try { await until(this, "record", () => !!this.exit, 1500); }
      catch { this.child.kill(); await until(this, "record", () => !!this.exit, 2000); }
    }
    for (const socket of this.server?.clients ?? []) socket.terminate();
    if (this.server) await new Promise(resolve => this.server.close(resolve));
    assert.deepEqual(this.errors, []); this.assertBudget();
  }
}
