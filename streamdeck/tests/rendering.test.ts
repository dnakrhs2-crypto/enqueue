import test from "node:test";
import assert from "node:assert/strict";
import { EventEmitter } from "node:events";
import { performance } from "node:perf_hooks";
import { KeyRenderer, keyImage, xml, type KeyOutput, type KeyVisual } from "../src/ui/key-renderer.js";
import { until } from "./helpers.mjs";

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
