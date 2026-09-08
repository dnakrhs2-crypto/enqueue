import { performance } from "node:perf_hooks";
import { translator, type Language } from "./i18n.js";

export interface KeyOutput {
  setState(state: 0 | 1): Promise<void>;
  setImage(image: string, options?: { state?: number; target?: 0 | 1 | 2 }): Promise<void>;
  setTitle(title: string, options?: { state?: number; target?: 0 | 1 | 2 }): Promise<void>;
  showAlert(): Promise<void>;
}
export type KeyVisual = { state: 0 | 1; image: string; title: string };
export type Lamp = "on" | "off" | "muted-on" | "muted-off" | "disconnected" | "disabled" | "checking" | "version" | "missing" | "duplicate";
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
export const xml = (s: string): string => s.replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&apos;" })[c]!);
const images = new Map<string, string>();
export function keyImage(lamp: Lamp, language: Language, audioStopped = false): string {
  const key = `${lamp}/${language}/${audioStopped}`;
  const cached = images.get(key); if (cached) return cached;
  const t = translator(language), on = lamp === "on" || lamp === "muted-on", muted = lamp.startsWith("muted-");
  let shape: string, label: string;
  if (["on", "off", "muted-on", "muted-off"].includes(lamp)) {
    const color = muted ? "#FF5A5F" : on ? "#35D07F" : "#3A3F47";
    shape = `<circle cx="72" cy="41" r="27" fill="${color}" stroke="#AEB6C2" stroke-width="2"/><path d="M72 22v20m-11-13a17 17 0 1 0 22 0" stroke="#FFFFFF" stroke-width="4" fill="none"/>`;
    label = muted ? t(on ? "originalOn" : "originalOff") : on ? "ON" : "OFF";
    if (muted) shape += `<rect x="14" y="70" width="116" height="19" rx="6" fill="#FF5A5F"/><text x="72" y="84" font-size="13" fill="#15171B">${xml(t("muted"))}</text>`;
  } else {
    label = "";
    if (lamp === "disconnected") shape = '<path d="M27 19v24h29l10 10m51 23V55H88L78 45M42 31l-7 12m73 12-7 12" stroke="#AEB6C2" stroke-width="7" fill="none"/><path d="m71 22 10 10m-26 33 10 10" stroke="#FF5A5F" stroke-width="5"/>';
    else if (lamp === "disabled") shape = '<rect x="43" y="40" width="58" height="40" rx="6" fill="#3A3F47" stroke="#AEB6C2" stroke-width="4"/><path d="M54 40V29a18 18 0 0 1 36 0v11" fill="none" stroke="#AEB6C2" stroke-width="5"/>';
    else if (lamp === "missing" || lamp === "duplicate") shape = '<circle cx="72" cy="44" r="30" fill="none" stroke="#FFB454" stroke-width="4" stroke-dasharray="7 5"/><text x="72" y="57" font-size="36" fill="#FFB454">?</text>';
    else shape = '<circle cx="72" cy="44" r="27" fill="none" stroke="#4C8DFF" stroke-width="5"/><text x="72" y="56" fill="#FFFFFF" font-size="34">!</text>';
  }
  const pause = audioStopped ? `<path d="M122 8v14m9-14v14" stroke="#FFB454" stroke-width="4"><title>${xml(t("audioStopped"))}</title></path>` : "";
  const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="144" height="144" viewBox="0 0 144 144"><rect width="144" height="144" rx="12" fill="#15171B"/><g text-anchor="middle" font-family="Malgun Gothic,Segoe UI,sans-serif">${shape}${pause}<text x="72" y="${muted ? 105 : 87}" font-size="${muted ? 13 : 17}" fill="#FFFFFF">${xml(label)}</text></g></svg>`;
  const url = "data:image/svg+xml;base64," + Buffer.from(svg).toString("base64");
  images.set(key, url); // Only Lamp × language × audio (44 possible entries), never user names.
  return url;
}

export type ActionLamp = "all-on" | "all-off" | "mixed" | "group-muted" | "group-clear" | "plugin-on" | "plugin-off" | "status-on" | "status-off";
/** Only bounded state/count/index combinations are cached; user names remain host titles. */
export function actionImage(lamp: ActionLamp, language: Language, count = 0, total = 0, audioStopped = false): string {
  count = Math.max(0, Math.min(8, Math.trunc(count))); total = Math.max(0, Math.min(8, Math.trunc(total)));
  const cacheKey = `${lamp}/${language}/${count}/${total}/${audioStopped}`, cached = images.get(cacheKey);
  if (cached) return cached;
  const t = translator(language), muted = lamp === "group-muted", off = lamp.endsWith("off");
  const color = muted || lamp === "plugin-off" ? "#FF5A5F" : lamp === "mixed" ? "#FFB454" : lamp === "plugin-on" ? "#4C8DFF" : off ? "#3A3F47" : "#35D07F";
  let shape = `<circle cx="72" cy="35" r="23" fill="${color}" stroke="#AEB6C2" stroke-width="2"/>`;
  let label: string = t(off ? "off" : "on"), detail = "";
  if (lamp.startsWith("all") || lamp === "mixed") {
    shape = [0, 1, 2].map(i => `<circle cx="${36 + i * 36}" cy="35" r="14" fill="${lamp === "mixed" && i === 2 ? "#3A3F47" : color}" stroke="#AEB6C2" stroke-width="2"/>`).join("");
    label = t(lamp === "mixed" ? "someOn" : off ? "allOff" : "allOn"); detail = `${count}/${total}`;
  } else if (lamp.startsWith("group")) {
    shape = `<rect x="43" y="12" width="58" height="46" rx="10" fill="${color}" stroke="#AEB6C2" stroke-width="2"/>`
      + (muted ? '<path d="m50 18 44 34" stroke="#FFFFFF" stroke-width="5"/>' : '<path d="m54 34 12 12 23-23" fill="none" stroke="#FFFFFF" stroke-width="4"/>');
    label = t(muted ? "muteState" : "unmuteState"); detail = t("targets").replace("{count}", String(count));
  } else if (lamp.startsWith("plugin")) shape += `<text x="72" y="45" font-size="28" fill="#FFFFFF">${count}</text>`;
  else label = t(off ? "audioStopped" : "connected");
  const pause = audioStopped ? `<path d="M122 8v14m9-14v14" stroke="#FFB454" stroke-width="4"><title>${xml(t("audioStopped"))}</title></path>` : "";
  const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="144" height="144" viewBox="0 0 144 144"><rect width="144" height="144" rx="12" fill="#15171B"/><g text-anchor="middle" font-family="Malgun Gothic,Segoe UI,sans-serif">${shape}${pause}<text x="72" y="79" font-size="16" fill="#FFFFFF">${xml(label)}</text><text x="72" y="98" font-size="14" fill="#FFFFFF">${xml(detail)}</text></g></svg>`;
  const url = "data:image/svg+xml;base64," + Buffer.from(svg).toString("base64"); images.set(cacheKey, url); return url;
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
