import { isObject, isUuid, type Channel, type Fx, type Snapshot } from "./protocol.js";

export type MicSettings = {
  settingsVersion: 1;
  channelId: string;
  channelName: string;
  mode: "toggle" | "on" | "off";
  nameFallback: boolean;
  shortTitle: string;
};
export function migrateSettings(value: unknown): { settings: MicSettings; valid: boolean; changed: boolean } {
  const o = isObject(value) ? value : {};
  const valid = (o.settingsVersion === undefined || o.settingsVersion === 1)
    && (o.mode === undefined || o.mode === "toggle" || o.mode === "on" || o.mode === "off")
    && (o.nameFallback === undefined || typeof o.nameFallback === "boolean");
  const settings: MicSettings = {
    settingsVersion: 1,
    channelId: isUuid(o.channelId) ? o.channelId : "",
    channelName: typeof o.channelName === "string" ? o.channelName : "",
    mode: o.mode === "on" || o.mode === "off" ? o.mode : "toggle",
    nameFallback: typeof o.nameFallback === "boolean" ? o.nameFallback : true,
    shortTitle: typeof o.shortTitle === "string" ? o.shortTitle.slice(0, 64) : ""
  };
  const changed = valid && Object.entries(settings).some(([k, v]) => o[k] !== v);
  return { settings, valid, changed };
}
export type BindingResult = { status: "bound"; channel: Channel; renamedOrigin?: string }
  | { status: "missing" | "duplicate" };

/** Stored UUID/name are the origin; resolvedId belongs only to the current instance/session. */
class NamedBinding<T extends { id: string; name: string }> {
  private boundary = "";
  private resolvedId: string | undefined;
  private failure: "missing" | "duplicate" = "missing";
  constructor(private settings: { id: string; name: string; nameFallback: boolean }, private readonly items: (snapshot: Snapshot) => T[]) {}
  update(settings: { id: string; name: string; nameFallback: boolean }): void {
    if (settings.id !== this.settings.id) {
      this.boundary = ""; this.resolvedId = undefined;
    }
    this.settings = settings;
  }
  resolve(snapshot: Snapshot): { status: "bound"; item: T; renamedOrigin?: string } | { status: "missing" | "duplicate" } {
    const key = `${snapshot.instanceId}/${snapshot.sessionId}`, channels = this.items(snapshot);
    const origin = channels.find(c => c.id === this.settings.id);
    if (key !== this.boundary) {
      this.boundary = key; this.resolvedId = origin?.id; this.failure = "missing";
      if (!origin && isUuid(this.settings.id) && this.settings.nameFallback && this.settings.name) {
        const matches = channels.filter(c => c.name === this.settings.name);
        if (matches.length === 1) this.resolvedId = matches[0]!.id;
        else if (matches.length > 1) this.failure = "duplicate";
      }
    }
    if (origin) {
      this.resolvedId = origin.id;
      const renamedOrigin = origin.name !== this.settings.name ? origin.name : undefined;
      if (renamedOrigin !== undefined) this.settings = { ...this.settings, name: renamedOrigin };
      return { status: "bound", item: origin, ...(renamedOrigin !== undefined ? { renamedOrigin } : {}) };
    }
    const channel = channels.find(c => c.id === this.resolvedId);
    if (channel) return { status: "bound", item: channel };
    if (this.resolvedId) { this.resolvedId = undefined; this.failure = "missing"; }
    return { status: this.failure };
  }
}

type ChannelSettings = Pick<MicSettings, "channelId" | "channelName" | "nameFallback">;
export class MicBinding {
  private readonly binding: NamedBinding<Channel>;
  constructor(settings: ChannelSettings) { this.binding = new NamedBinding({ id: settings.channelId, name: settings.channelName, nameFallback: settings.nameFallback }, s => s.state.channels); }
  update(settings: ChannelSettings): void { this.binding.update({ id: settings.channelId, name: settings.channelName, nameFallback: settings.nameFallback }); }
  resolve(snapshot: Snapshot): BindingResult {
    const r = this.binding.resolve(snapshot); return r.status === "bound" ? { ...r, channel: r.item } : r;
  }
}
type FxSettings = { fxId: string; fxName: string; nameFallback: boolean };
export class FxBinding {
  private readonly binding: NamedBinding<Fx>;
  constructor(settings: FxSettings) { this.binding = new NamedBinding({ id: settings.fxId, name: settings.fxName, nameFallback: settings.nameFallback }, s => s.state.fx); }
  update(settings: FxSettings): void { this.binding.update({ id: settings.fxId, name: settings.fxName, nameFallback: settings.nameFallback }); }
  resolve(snapshot: Snapshot): { status: "bound"; fx: Fx; renamedOrigin?: string } | { status: "missing" | "duplicate" } {
    const r = this.binding.resolve(snapshot); return r.status === "bound" ? { ...r, fx: r.item } : r;
  }
}

export type ActionKind = "all-mics" | "mic-mute-group" | "fx-mute-group" | "plugin-group" | "fx-send" | "fx-send-step" | "status";
export type ActionSettings = Omit<MicSettings, "mode"> & {
  mode: "toggle" | "on" | "off" | "mute" | "unmute" | "up" | "down" | "set";
  fxId: string; fxName: string; groupIndex: number;
  stepPercent: 1 | 5 | 10; targetPercent: number; pressMode: "pre-post" | "none"; display: "connection" | "session" | "audio";
};
export function actionSettings(kind: ActionKind, value: unknown): { settings: ActionSettings; valid: boolean; changed: boolean } {
  const o = isObject(value) ? value : {}, mute = kind.endsWith("mute-group"), sendStep = kind === "fx-send-step";
  const modes = sendStep ? ["up", "down", "set"] : mute ? ["toggle", "mute", "unmute"] : ["toggle", "on", "off"];
  const steps = sendStep ? [1, 5, 10] : [1, 5];
  const validTarget = typeof o.targetPercent === "number" && Number.isInteger(o.targetPercent) && o.targetPercent >= 0 && o.targetPercent <= 100;
  const base = migrateSettings({ ...o, mode: "toggle" });
  const valid = base.valid && (o.mode === undefined || modes.includes(String(o.mode)))
    && (kind !== "plugin-group" || o.groupIndex === undefined || (Number.isInteger(o.groupIndex) && Number(o.groupIndex) >= 1 && Number(o.groupIndex) <= 5))
    && (!(kind === "fx-send" || sendStep) || o.stepPercent === undefined || steps.includes(o.stepPercent as number))
    && (kind !== "fx-send" || o.pressMode === undefined || o.pressMode === "pre-post" || o.pressMode === "none")
    && (!sendStep || o.targetPercent === undefined || validTarget)
    && (kind !== "status" || o.display === undefined || ["connection", "session", "audio"].includes(String(o.display)));
  const settings: ActionSettings = { ...base.settings,
    mode: modes.includes(String(o.mode)) ? o.mode as ActionSettings["mode"] : sendStep ? "up" : "toggle",
    fxId: isUuid(o.fxId) ? o.fxId : "", fxName: typeof o.fxName === "string" ? o.fxName : "",
    groupIndex: Number.isInteger(o.groupIndex) && Number(o.groupIndex) >= 1 && Number(o.groupIndex) <= 5 ? Number(o.groupIndex) : 1,
    stepPercent: steps.includes(o.stepPercent as number) ? o.stepPercent as ActionSettings["stepPercent"] : sendStep ? 5 : 1,
    targetPercent: validTarget ? o.targetPercent as number : 50, pressMode: o.pressMode === "none" ? "none" : "pre-post",
    display: o.display === "session" || o.display === "audio" ? o.display : "connection" };
  return { settings, valid, changed: valid && Object.entries(settings).some(([k, v]) => o[k] !== v) };
}
