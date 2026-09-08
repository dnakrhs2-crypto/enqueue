import streamDeck, { SingletonAction, type KeyAction, type DialAction, type WillAppearEvent, type WillDisappearEvent, type DidReceiveSettingsEvent, type KeyDownEvent, type SendToPluginEvent, type PropertyInspectorDidAppearEvent, type PropertyInspectorDidDisappearEvent } from "@elgato/streamdeck";
import type { JsonValue } from "@elgato/utils";
import { ConnectionError, type LiveMixConnection } from "../livemix/connection.js";
import { MicBinding, FxBinding, actionSettings, type ActionKind, type ActionSettings } from "../livemix/bindings.js";
import type { CommandQueue } from "../livemix/command-queue.js";
import { isObject, type Channel, type Fx, type Snapshot } from "../livemix/protocol.js";
import { KeyRenderer, keyImage, statusTitle, type Lamp } from "../ui/key-renderer.js";
import { FeedbackRenderer } from "../ui/feedback.js";
import { translator, type Language, type StringKey } from "../ui/i18n.js";

export type ActionContext = {
  id: string; device: string; handle: KeyAction<ActionSettings> | DialAction<ActionSettings>;
  key?: KeyRenderer; feedback?: FeedbackRenderer; binding: MicBinding; fxBinding: FxBinding;
  settings: ActionSettings; valid: boolean; generation: number; error?: string; problem?: string; saveQueued?: boolean;
  press?: { at: number; rotated: boolean; session: string };
};

/** The mic action's lifecycle, binding, PI and renderer pattern shared by Round 3 actions. */
export abstract class LiveMixAction extends SingletonAction<ActionSettings> {
  protected readonly contexts = new Map<string, ActionContext>();
  protected readonly t;
  private readonly bindings = new Map<string, { mic: MicBinding; fx: FxBinding }>();
  private sessionKey = "";
  private readonly history = new Map<string, { calls: number[]; expiry?: ReturnType<typeof setTimeout> }>();
  private inspector: { context: string; requestId: string; sequence: number } | undefined;
  constructor(protected readonly connection: LiveMixConnection, protected readonly queue: CommandQueue, protected readonly language: Language,
    protected readonly kind: ActionKind, private readonly capability?: string) {
    super(); this.t = translator(language);
    connection.store.on("change", () => this.refresh()); connection.on("status", () => this.refresh());
    connection.store.on("boundary", () => { for (const c of this.contexts.values()) c.press = undefined; });
    streamDeck.devices.onDeviceDidDisconnect(ev => { for (const [id, c] of this.contexts) if (c.device === ev.device.id) this.remove(id); });
  }
  override onWillAppear(ev: WillAppearEvent<ActionSettings>): void {
    if ((this.kind === "fx-send") !== ev.action.isDial()) return;
    this.remove(ev.action.id);
    const migrated = actionSettings(this.kind, ev.payload.settings), settings = migrated.settings;
    const history = this.history.get(ev.action.id) ?? { calls: [] }; clearTimeout(history.expiry); this.history.set(ev.action.id, history);
    const binding = this.bindings.get(ev.action.id) ?? { mic: new MicBinding(settings), fx: new FxBinding(settings) };
    binding.mic.update(settings); binding.fx.update(settings); this.bindings.set(ev.action.id, binding);
    const c: ActionContext = { id: ev.action.id, device: ev.action.device.id, handle: ev.action,
      ...(ev.action.isKey() ? { key: new KeyRenderer(ev.action, history.calls) } : { feedback: new FeedbackRenderer(ev.action, history.calls) }),
      binding: binding.mic, fxBinding: binding.fx, settings, valid: migrated.valid, generation: 0 };
    this.contexts.set(c.id, c); this.render(c);
    if (migrated.changed) void c.handle.setSettings(settings).catch(() => {});
  }
  override onWillDisappear(ev: WillDisappearEvent<ActionSettings>): void { this.remove(ev.action.id); }
  private remove(id: string): void {
    const c = this.contexts.get(id); c?.key?.dispose(); c?.feedback?.dispose(); this.contexts.delete(id); this.queue.cancelOwner(id);
    const history = this.history.get(id);
    if (history) { clearTimeout(history.expiry); history.expiry = setTimeout(() => this.history.delete(id), 1001); history.expiry.unref(); }
    if (this.inspector?.context === id) this.inspector = undefined;
  }
  override onDidReceiveSettings(ev: DidReceiveSettingsEvent<ActionSettings>): void {
    const c = this.contexts.get(ev.action.id); if (!c) return;
    const migrated = actionSettings(this.kind, ev.payload.settings);
    if (JSON.stringify(c.settings) !== JSON.stringify(migrated.settings) || c.valid !== migrated.valid) {
      c.generation++; c.press = undefined; this.queue.cancelOwner(c.id); c.error = undefined;
    }
    c.settings = migrated.settings; c.valid = migrated.valid; c.binding.update(c.settings); c.fxBinding.update(c.settings);
    this.render(c); this.sendOptions();
    if (migrated.changed) void c.handle.setSettings(c.settings).catch(() => {});
  }
  override onKeyDown(ev: KeyDownEvent<ActionSettings>): void {
    const c = this.contexts.get(ev.action.id); if (c?.key) this.run(c, snapshot => this.pressKey(c, snapshot));
  }
  // No keyUp or touch handlers: release, tap and long touch never issue key commands.
  protected pressKey(_c: ActionContext, _snapshot: Snapshot): Promise<void> | void {}
  protected abstract draw(c: ActionContext, snapshot: Snapshot): void;
  protected run(c: ActionContext, input: (snapshot: Snapshot) => Promise<void> | void): void {
    const generation = c.generation;
    const failed = (error: unknown): void => {
      if (this.contexts.get(c.id) === c && c.generation === generation) this.fail(c, error);
    };
    try {
      if (!c.valid) throw new ConnectionError("INVALID_SETTINGS");
      if (!this.connection.ready || !this.connection.store.snapshot) throw new ConnectionError("DISCONNECTED");
      if (this.capability && !this.connection.capabilities.includes(this.capability)) throw new ConnectionError("UNSUPPORTED");
      c.error = undefined;
      const pending = input(this.connection.store.snapshot); this.render(c); this.sendOptions();
      if (pending) void pending.catch(failed);
    } catch (error) { failed(error); }
  }
  protected channel(c: ActionContext, snapshot: Snapshot): Channel {
    const r = c.binding.resolve(snapshot);
    if (r.status !== "bound") throw new ConnectionError(r.status === "duplicate" ? "DUPLICATE_NAME" : "CHANNEL_NOT_FOUND");
    if (r.renamedOrigin !== undefined) this.rename(c, { channelName: r.renamedOrigin });
    return r.channel;
  }
  protected fx(c: ActionContext, snapshot: Snapshot): Fx {
    const r = c.fxBinding.resolve(snapshot);
    if (r.status !== "bound") throw new ConnectionError(r.status === "duplicate" ? "DUPLICATE_NAME" : "FX_NOT_FOUND");
    if (r.renamedOrigin !== undefined) this.rename(c, { fxName: r.renamedOrigin });
    return r.fx;
  }
  private rename(c: ActionContext, patch: Partial<ActionSettings>): void {
    c.settings = { ...c.settings, ...patch };
    if (c.saveQueued) return;
    c.saveQueued = true;
    // Channel and FX can be renamed in one snapshot; persist their names together.
    queueMicrotask(() => {
      c.saveQueued = false;
      if (this.contexts.get(c.id) === c) void c.handle.setSettings(c.settings).catch(() => {});
    });
  }
  private errorKey(error: unknown): StringKey {
    const code = error instanceof ConnectionError ? error.code : "COMMAND_FAILED";
    if (code === "DISCONNECTED") return this.connection.status === "disabled" ? "disabled" : "disconnected";
    const keys: Record<string, StringKey> = { INVALID_SETTINGS: "invalidSettings", CHANNEL_NOT_FOUND: "missing", FX_NOT_FOUND: "missingFx",
      SEND_NOT_FOUND: "missingFx", GROUP_NOT_FOUND: "missingGroup", PLUGIN_GROUP_NOT_FOUND: "missingGroup", NO_CHANNELS: "noMics", DUPLICATE_NAME: "duplicate",
      INPUT_BUSY: "delayed", INPUT_EXPIRED: "delayed", REVISION_CONFLICT: "conflict", UNSUPPORTED: "unsupported" };
    return keys[code] ?? "failure";
  }
  private fail(c: ActionContext, error: unknown): void {
    c.error = this.t(this.errorKey(error)); this.render(c); c.key?.alert(); c.feedback?.alert(); this.sendOptions();
  }
  private problem(c: ActionContext, title: string, lamp: Lamp): void {
    c.problem = title;
    c.key?.render({ state: 0, image: keyImage(lamp, this.language), title: statusTitle(title) });
    c.feedback?.render({ name: "LiveMix", amount: "—", mode: "", level: { value: 0, enabled: false }, status: { value: title, font: { size: 16 } } });
  }
  protected render(c: ActionContext): void {
    c.problem = undefined;
    if (!this.connection.ready || !this.connection.store.snapshot) {
      const status = this.connection.status;
      const lamp = status === "connecting" || status === "ready" ? "checking" : status;
      this.problem(c, this.t(lamp), lamp); return;
    }
    try {
      if (!c.valid) throw new ConnectionError("INVALID_SETTINGS");
      if (this.capability && !this.connection.capabilities.includes(this.capability)) throw new ConnectionError("UNSUPPORTED");
      this.draw(c, this.connection.store.snapshot);
    } catch (error) { this.problem(c, this.t(this.errorKey(error)), "missing"); }
  }
  private refresh(): void {
    const snapshot = this.connection.store.snapshot;
    if (snapshot) {
      const key = `${snapshot.instanceId}/${snapshot.sessionId}`;
      if (key !== this.sessionKey) {
        this.sessionKey = key;
        for (const id of this.bindings.keys()) if (!this.contexts.has(id)) this.bindings.delete(id);
      }
    }
    for (const c of this.contexts.values()) this.render(c); this.sendOptions();
  }
  override onPropertyInspectorDidAppear(ev: PropertyInspectorDidAppearEvent<ActionSettings>): void {
    if (this.contexts.has(ev.action.id)) this.inspector = { context: ev.action.id, requestId: "", sequence: 0 };
  }
  override onPropertyInspectorDidDisappear(ev: PropertyInspectorDidDisappearEvent<ActionSettings>): void {
    if (this.inspector?.context === ev.action.id) this.inspector = undefined;
  }
  override onSendToPlugin(ev: SendToPluginEvent<JsonValue, ActionSettings>): void {
    if (!isObject(ev.payload) || ev.payload.op !== "getOptions" || typeof ev.payload.requestId !== "string" || ev.payload.requestId.length > 64
      || !this.inspector || ev.action.id !== this.inspector.context || streamDeck.ui.action?.id !== ev.action.id) return;
    this.inspector.requestId = ev.payload.requestId; this.sendOptions();
  }
  protected sendOptions(): void {
    const pi = this.inspector;
    if (!pi || !pi.requestId || streamDeck.ui.action?.id !== pi.context) return;
    const c = this.contexts.get(pi.context); if (!c) return;
    const snapshot = this.connection.store.snapshot, ready = this.connection.ready && !!snapshot;
    const binding = ready ? c.binding.resolve(snapshot) : undefined;
    void streamDeck.ui.sendToPropertyInspector({ op: "options", context: pi.context, requestId: pi.requestId, sequence: ++pi.sequence,
      connection: ready ? "ready" : this.connection.status === "ready" ? "checking" : this.connection.status, language: this.language, settings: c.settings,
      message: ready ? c.error ?? c.problem ?? this.t("connected")
        : (c.error ? c.error + "\n" : "") + this.t(this.connection.status === "disabled" ? "disabledHelp" : "offlineHelp"),
      ...(ready ? { instanceId: snapshot.instanceId, sessionId: snapshot.sessionId, revision: snapshot.revision,
        channels: snapshot.state.channels.map(ch => ({ id: ch.id, name: ch.name, groupIndices: ch.pluginGroups.map(g => g.index) })),
        fx: snapshot.state.fx.map(fx => ({ id: fx.id, name: fx.name })),
        groupIndices: binding?.status === "bound" ? binding.channel.pluginGroups.map(g => g.index) : [],
        muteGroupCounts: { mic: snapshot.state.channels.filter(ch => ch.muteGroup).length, fx: snapshot.state.fx.filter(fx => fx.muteGroup).length }
      } : {}) }).catch(() => {});
  }
}
