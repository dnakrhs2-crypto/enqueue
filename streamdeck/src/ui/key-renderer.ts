import { performance } from "node:perf_hooks";
import type { Language } from "./i18n.js";
import { strings } from "./strings.js";
import { micSvg, sendSvg, actionSvg, type Lamp, type ActionLamp } from "./artwork.js";
export { xml } from "./artwork.js";
export type { Lamp, ActionLamp } from "./artwork.js";

export interface KeyOutput {
  setState(state: 0 | 1): Promise<void>;
  setImage(image: string, options?: { state?: number; target?: 0 | 1 | 2 }): Promise<void>;
  setTitle(title: string, options?: { state?: number; target?: 0 | 1 | 2 }): Promise<void>;
  showAlert(): Promise<void>;
}
export type KeyVisual = { state: 0 | 1; image: string; title: string };
export function displayName(name: string): string {
  const chars = Array.from(name.replace(/[\u0000-\u001f\u007f-\u009f]/g, " ").trim());
  return chars.length <= 10 ? chars.join("") : chars.slice(0, 10).join("") + "\n" + (chars.length <= 20 ? chars.slice(10).join("") : chars.slice(10, 19).join("") + "…");
}
export function statusTitle(title: string): string {
  if (title.startsWith("LiveMix ")) return title.replace("LiveMix ", "LiveMix\n");
  if (title.length <= 10) return title;
  const words = title.split(" "), lines = [""];
  for (const word of words) {
    const last = lines.length - 1;
    if (lines[last] && lines[last]!.length + word.length + 1 > 13 && lines.length < 2) lines.push(word);
    else lines[last] += (lines[last] ? " " : "") + word;
  }
  return lines.join("\n");
}
const images = new Map<string, string>();
const dataUrl = (svg: string): string => "data:image/svg+xml;base64," + Buffer.from(svg).toString("base64");
export function keyImage(lamp: Lamp, language: Language, audioStopped = false, unknownValue = false): string {
  const key = [lamp, language, audioStopped, unknownValue].join("/");
  const cached = images.get(key); if (cached) return cached;
  const image = dataUrl(micSvg(lamp, strings[language], audioStopped, unknownValue));
  images.set(key, image); return image;
}

/** Amount/badge combinations are not cached; KeyRenderer deduplicates output. */
export function sendImage(percent: number, badge: string, language: Language, muted = false, audioStopped = false): string {
  return dataUrl(sendSvg(percent, badge, strings[language], muted, audioStopped));
}

/** Cache only bounded state/count/index/group combinations, never user names. */
export function actionImage(lamp: ActionLamp, language: Language, count = 0, total = 0, audioStopped = false, group: "mic" | "fx" = "mic", index = 1): string {
  count = Number.isFinite(count) ? Math.max(0, Math.min(8, Math.trunc(count))) : 0;
  total = Number.isFinite(total) ? Math.max(0, Math.min(8, Math.trunc(total))) : 0;
  index = Number.isFinite(index) ? Math.max(1, Math.min(5, Math.trunc(index))) : 1;
  const key = [lamp, language, count, total, audioStopped, group, index].join("/");
  const cached = images.get(key); if (cached) return cached;
  const image = dataUrl(actionSvg(lamp, strings[language], count, total, audioStopped, group, 144, index));
  images.set(key, image); return image;
}

/** A coalesced update is sent state → image → title. The 10/s budget includes showAlert. */
export class KeyRenderer {
  private desired: KeyVisual | undefined;
  private sent: Partial<KeyVisual> = {};
  private timer: ReturnType<typeof setTimeout> | undefined;
  private sending = false;
  private disposed = false;
  private alertPending = false;
  constructor(private readonly output: KeyOutput, private readonly calls: number[] = []) {}
  render(visual: KeyVisual): void { this.desired = visual; void this.flush(); }
  alert(): void { this.alertPending = true; void this.flush(); }
  dispose(): void { this.disposed = true; clearTimeout(this.timer); }
  private async flush(): Promise<void> {
    if (this.disposed || this.sending || this.timer) return;
    this.sending = true;
    try {
      while (!this.disposed) {
        const v = this.desired; if (!v) break;
        const stateChanged = this.sent.state !== v.state;
        const imageChanged = this.sent.image !== v.image;
        const titleChanged = this.sent.title !== v.title;
        const showAlert = this.alertPending;
        const count = Number(stateChanged) + Number(imageChanged) + Number(titleChanged) + Number(showAlert);
        if (!count) break;
        const now = performance.now();
        while (this.calls.length && now - this.calls[0]! >= 1000) this.calls.shift();
        if (this.calls.length + count > 10) {
          this.timer = setTimeout(() => { this.timer = undefined; void this.flush(); }, Math.max(1, 1001 - (now - this.calls[0]!)));
          break;
        }
        const invoke = async (fn: () => Promise<void>): Promise<void> => { this.calls.push(performance.now()); await fn(); };
        if (stateChanged) { await invoke(() => this.output.setState(v.state)); this.sent.state = v.state; }
        if (this.disposed) break;
        // Omitting state sets both host states; identical values need not be resent on a state switch.
        if (imageChanged) { await invoke(() => this.output.setImage(v.image, { target: 0 })); this.sent.image = v.image; }
        if (this.disposed) break;
        if (titleChanged) { await invoke(() => this.output.setTitle(v.title, { target: 0 })); this.sent.title = v.title; }
        if (this.disposed) break;
        if (showAlert) { this.alertPending = false; await invoke(() => this.output.showAlert()); }
      }
    } catch { this.sent = {}; /* Host may have discarded the context. Next host/state event retries. */ }
    finally { this.sending = false; }
  }
}
