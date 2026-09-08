import { isObject, isUuid, type Channel, type Snapshot } from "./protocol.js";

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
export class MicBinding {
  private boundary = "";
  private resolvedId: string | undefined;
  private failure: "missing" | "duplicate" = "missing";
  constructor(private settings: MicSettings) {}
  update(settings: MicSettings): void {
    if (settings.channelId !== this.settings.channelId) {
      this.boundary = ""; this.resolvedId = undefined;
    }
    this.settings = settings;
  }
  resolve(snapshot: Snapshot): BindingResult {
    const key = `${snapshot.instanceId}/${snapshot.sessionId}`, channels = snapshot.state.channels;
    const origin = channels.find(c => c.id === this.settings.channelId);
    if (key !== this.boundary) {
      this.boundary = key; this.resolvedId = origin?.id; this.failure = "missing";
      if (!origin && isUuid(this.settings.channelId) && this.settings.nameFallback && this.settings.channelName) {
        const matches = channels.filter(c => c.name === this.settings.channelName);
        if (matches.length === 1) this.resolvedId = matches[0]!.id;
        else if (matches.length > 1) this.failure = "duplicate";
      }
    }
    if (origin) {
      this.resolvedId = origin.id;
      const renamedOrigin = origin.name !== this.settings.channelName ? origin.name : undefined;
      if (renamedOrigin !== undefined) this.settings = { ...this.settings, channelName: renamedOrigin };
      return { status: "bound", channel: origin, ...(renamedOrigin !== undefined ? { renamedOrigin } : {}) };
    }
    const channel = channels.find(c => c.id === this.resolvedId);
    if (channel) return { status: "bound", channel };
    if (this.resolvedId) { this.resolvedId = undefined; this.failure = "missing"; }
    return { status: this.failure };
  }
}
