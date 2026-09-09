import test from "node:test";
import assert from "node:assert/strict";
import { EventEmitter } from "node:events";
import { performance } from "node:perf_hooks";
import { KeyRenderer, keyImage, actionImage, xml, type KeyOutput, type KeyVisual } from "../src/ui/key-renderer.js";
import { FeedbackRenderer } from "../src/ui/feedback.js";
import { strings } from "../src/ui/strings.js";
import { micSvg, actionSvg, sendSvg, glyph } from "../src/ui/artwork.js";
import { readFile } from "node:fs/promises";
import { resolve } from "node:path";
import { until, pluginRoot } from "./helpers.mjs";

class Output extends EventEmitter implements KeyOutput {
  calls: { method: string; at: number; value?: unknown }[] = [];
  async record(method: string, value?: unknown): Promise<void> { this.calls.push({ method, at: performance.now(), value }); this.emit("call"); }
  setState(value: 0 | 1): Promise<void> { return this.record("state", value); }
  setImage(value: string): Promise<void> { return this.record("image", value); }
  setTitle(value: string): Promise<void> { return this.record("title", value); }
  showAlert(): Promise<void> { return this.record("alert"); }
}
const on: KeyVisual = { state: 1, image: keyImage("on", "ko"), title: "곰 마이크" };
test("renderer sends only changed values in state/image/title order and shares images for identical templates", async t => {
  const output = new Output(), renderer = new KeyRenderer(output); t.after(() => renderer.dispose());
  renderer.render(on); await until(output, "call", () => output.calls.length === 3);
  assert.deepEqual(output.calls.map(c => c.method), ["state", "image", "title"]);
  renderer.render(on); renderer.alert(); await until(output, "call", () => output.calls.some(c => c.method === "alert"));
  assert.deepEqual(output.calls.map(c => c.method), ["state", "image", "title", "alert"]);
  assert.equal(keyImage("on", "ko"), on.image); assert.equal(xml('<&"\''), "&lt;&amp;&quot;&apos;");
});

test("release images have localized labels, distinct group shapes and independent state colors", () => {
  for (const language of ["ko", "en"] as const) {
    const t = strings[language];
    for (const [lamp, label, color] of [["mixed", t.someOn, "#FFB454"], ["group-muted", t.muteState, "#FF5A5F"],
      ["group-clear", t.unmuteState, "#35D07F"], ["plugin-on", t.on, "#4C8DFF"], ["plugin-off", t.off, "#FF5A5F"],
      ["plugin-all-on", t.allOn, "#4C8DFF"], ["plugin-all-off", t.allOff, "#FF5A5F"], ["plugin-all-mixed", t.someOn, "#FFB454"]] as const) {
      const image = actionImage(lamp, language, 2, 3, true), svg = Buffer.from(image.split(",")[1]!, "base64").toString("utf8");
      assert.ok(svg.includes(label) && svg.includes(color) && svg.includes(t.audioStopped));
      if (lamp === "mixed" || lamp.startsWith("plugin-all")) assert.ok(svg.includes("2/3"));
      assert.equal(actionImage(lamp, language, 2, 3, true), image);
    }
    assert.notEqual(actionImage("group-muted", language, 2, 0, false, "mic"), actionImage("group-muted", language, 2, 0, false, "fx"));
    assert.notEqual(actionImage("plugin-all-on", language, 2, 2, false, "mic", 1), actionImage("plugin-all-on", language, 2, 2, false, "mic", 2));
  }
});

test("nine manifest actions have required controllers/states, translated triggers and all assets", async () => {
  const manifest = JSON.parse(await readFile(resolve(pluginRoot, "manifest.json"), "utf8"));
  assert.equal(manifest.Version, "1.1.0.0");
  // The actual static previews and runtime rendering share the same geometry.
  const samples = [
    ["mic/on", micSvg("on", strings.en)],
    ["all-mics/mixed", actionSvg("mixed", strings.en, 2, 3)],
    ["fx-mute-group/on", actionSvg("group-muted", strings.en, 2, 0, false, "fx")],
    ["fx-send-step/increase", sendSvg(35, "+5", strings.en)]
  ];
  for (const [path, expected] of samples) assert.equal((await readFile(resolve(pluginRoot, "imgs/actions/" + path + "@2x.svg"), "utf8")).trim(), expected);
  for (const [name, lamp, count] of [["off", "plugin-all-off", 0], ["on", "plugin-all-on", 3], ["mixed", "plugin-all-mixed", 2]] as const) {
    for (const [suffix, size] of [["", 72], ["@2x", 144]] as const) for (const index of [1, 2, 3, 4, 5]) {
      const expected = actionSvg(lamp, strings.en, count, 3, false, "mic", size, index);
      assert.equal((await readFile(resolve(pluginRoot, `imgs/actions/plugin-group-all/group-${index}-${name}${suffix}.svg`), "utf8")).trim(), expected);
      if (index === 1) assert.equal((await readFile(resolve(pluginRoot, `imgs/actions/plugin-group-all/${name}${suffix}.svg`), "utf8")).trim(), expected);
      if (size === 144) assert.equal(Buffer.from(actionImage(lamp, "en", count, 3, false, "mic", index).split(",")[1]!, "base64").toString("utf8"), expected);
    }
  }
  assert.notEqual(glyph("plugin-group-all"), glyph("plugin-group"), "Distinct white menu glyph");
  assert.equal(manifest.Actions.length, 9); assert.equal(new Set(manifest.Actions.map((a: any) => a.UUID)).size, 9);
  const ko = JSON.parse(await readFile(resolve(pluginRoot, "ko.json"), "utf8")), en = JSON.parse(await readFile(resolve(pluginRoot, "en.json"), "utf8"));
  assert.deepEqual(Object.keys(ko.Localization).sort(), Object.keys(en.Localization).sort());
  for (const a of manifest.Actions) {
    const dial = a.UUID.endsWith(".fx-send"), sendStep = a.UUID.endsWith(".fx-send-step");
    assert.deepEqual(a.Controllers, [dial ? "Encoder" : "Keypad"]);
    assert.equal(a.States.length, dial || sendStep ? 1 : 2);
    if (sendStep) { assert.equal(a.Name, "FX Send ±"); assert.equal(ko[a.UUID].Name, "FX 보내는 양 ±"); }
    if (a.UUID.endsWith(".plugin-group-all")) {
      assert.equal(a.Name, "Plugin Group (All Mics)"); assert.equal(ko[a.UUID].Name, "플러그인 그룹 (전체 마이크)");
      assert.deepEqual(a.States.map((s: any) => s.Name), ["All OFF", "All ON"]);
    }
    for (const property of ["DisableAutomaticStates", "DisableCaching"]) assert.equal(a[property], true);
    for (const property of ["UserTitleEnabled", "SupportedInMultiActions", "SupportedInKeyLogicActions"]) assert.equal(a[property], false);
    for (const locale of [ko, en]) { assert.ok(locale[a.UUID].Name); assert.ok(locale[a.UUID].Tooltip); assert.ok(locale[a.UUID].States.every((s: any) => s.Name)); }
    for (const [path, size] of [[a.Icon, 20], ...a.States.map((s: any) => [s.Image, 72]), ...(dial ? [[a.Encoder.Icon, 72]] : [])]) {
      for (const [suffix, scale] of [["", 1], ["@2x", 2]] as const) {
        const svg = await readFile(resolve(pluginRoot, path + suffix + ".svg"), "utf8"); assert.ok(svg.includes(`width="${size * scale}"`));
      }
    }
    if (dial) for (const locale of [ko, en]) for (const trigger of ["Push", "Rotate", "Touch", "LongTouch"]) assert.ok(locale[a.UUID].Encoder.TriggerDescription[trigger]);
  }
  const layout = JSON.parse(await readFile(resolve(pluginRoot, "layouts/fx-send.json"), "utf8"));
  assert.deepEqual(Object.fromEntries(layout.items.map((item: any) => [item.key, item.rect])), {
    name: [4, 0, 192, 24], amount: [4, 24, 116, 38], mode: [124, 24, 72, 38], level: [4, 66, 192, 10], status: [4, 80, 192, 20] });
  assert.equal(layout.items.find((i: any) => i.key === "amount").value, "—");
  assert.equal(layout.items.find((i: any) => i.key === "level").enabled, false);
  assert.equal(layout.items.find((i: any) => i.key === "name")["text-overflow"], "ellipsis");
});

test("feedback renderer coalesces updates and alerts, avoids duplicates and cancels deferred output on disposal", async t => {
  const events = new EventEmitter(), calls: { at: number; value: unknown }[] = [];
  const record = async (value: unknown): Promise<void> => { calls.push({ at: performance.now(), value }); events.emit("call"); };
  const renderer = new FeedbackRenderer({ setFeedback: record, showAlert: () => record("alert") }); t.after(() => renderer.dispose());
  renderer.render({ amount: "35%" }); await until(events, "call", () => calls.length === 1);
  renderer.render({ amount: "35%" }); renderer.alert(); await until(events, "call", () => calls.length === 2);
  assert.equal(calls[1]!.value, "alert");
  for (let n = 0; n < 15; n++) { renderer.render({ amount: `${n}%` }); await Promise.resolve(); }
  renderer.render({ amount: "—", level: { enabled: false } });
  await until(events, "call", () => JSON.stringify(calls.at(-1)!.value).includes("—"));
  for (const call of calls) assert.ok(calls.filter(c => c.at <= call.at && c.at > call.at - 1000).length <= 10);
  const count = calls.length; renderer.dispose(); renderer.alert(); renderer.render({ amount: "0%" }); await Promise.resolve(); assert.equal(calls.length, count);
});
test("coalesced renderer includes alerts and reappearing keys in the rolling 10/s budget", async t => {
  const output = new Output(), history: number[] = []; let renderer = new KeyRenderer(output, history); t.after(() => renderer.dispose());
  renderer.render(on); await until(output, "call", () => output.calls.length === 3);
  for (let n = 0; n < 3; n++) {
    renderer.dispose(); renderer = new KeyRenderer(output, history); renderer.render(on); renderer.alert();
    if (n < 2) await until(output, "call", () => output.calls.filter(c => c.method === "alert").length >= n + 1);
  }
  renderer.render({ state: 0, image: keyImage("disconnected", "ko"), title: "LiveMix 미연결" });
  await until(output, "call", () => output.calls.some(c => c.method === "title" && c.value === "LiveMix 미연결"), 4000);
  for (const call of output.calls) assert.ok(output.calls.filter(c => c.at <= call.at && c.at > call.at - 1000).length <= 10);
});
test("muted templates keep the original state text and never interpolate names into SVG", () => {
  for (const [lamp, original] of [["muted-on", "원래 ON"], ["muted-off", "원래 OFF"]] as const) {
    const svg = Buffer.from(keyImage(lamp, "ko", true).split(",")[1]!, "base64").toString("utf8");
    assert.ok(svg.includes("#FF5A5F") && svg.includes(original) && svg.includes("오디오 멈춤"));
  }
});
