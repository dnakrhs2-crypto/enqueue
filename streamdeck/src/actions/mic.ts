import streamDeck, { action, SingletonAction, type WillAppearEvent, type WillDisappearEvent, type DidReceiveSettingsEvent, type KeyDownEvent, type SendToPluginEvent, type PropertyInspectorDidAppearEvent, type PropertyInspectorDidDisappearEvent } from "@elgato/streamdeck";
import type { JsonValue } from "@elgato/utils";
import { LiveMixConnection, ConnectionError } from "../livemix/connection.js";
import { MicBinding, migrateSettings, type MicSettings } from "../livemix/bindings.js";
import { CommandQueue } from "../livemix/command-queue.js";
import { isObject } from "../livemix/protocol.js";
import { displayName, statusTitle, keyImage, KeyRenderer, type KeyOutput, type Lamp } from "../ui/key-renderer.js";
import { translator, type Language } from "../ui/i18n.js";

type KeyHandle = KeyOutput & { setSettings(settings: MicSettings): Promise<void> };
type KeyContext = { device: string; handle: KeyHandle; renderer: KeyRenderer; binding: MicBinding; settings: MicSettings; valid: boolean; error?: string };

@action({ UUID: "com.gomtwigim.livemix.mic" })
export class MicrophoneAction extends SingletonAction<MicSettings> {
  private readonly contexts = new Map<string, KeyContext>();
  // Keep resolution across profile/device visibility changes within this session. Disappearance
  // must not turn a deletion into a fresh opportunity for name fallback.
  private readonly bindings = new Map<string, MicBinding>();
  private sessionKey = "";
  private readonly callHistory = new Map<string, { calls: number[]; expiry?: ReturnType<typeof setTimeout> }>();
  private inspector: { context: string; requestId: string; sequence: number } | undefined;
  private readonly t;
  constructor(private readonly connection: LiveMixConnection, private readonly queue: CommandQueue, private readonly language: Language) {
    super(); this.t = translator(language);
    connection.store.on("change", () => this.refresh());
    connection.on("status", () => this.refresh());
    streamDeck.devices.onDeviceDidDisconnect(ev => {
      for (const [id, context] of this.contexts) if (context.device === ev.device.id) this.remove(id);
    });
  }
  override onWillAppear(ev: WillAppearEvent<MicSettings>): void {
    if (!ev.action.isKey()) return;
    this.remove(ev.action.id);
    const history = this.callHistory.get(ev.action.id) ?? { calls: [] };
    clearTimeout(history.expiry); this.callHistory.set(ev.action.id, history);
    const migrated = migrateSettings(ev.payload.settings), settings = migrated.settings;
    const binding = this.bindings.get(ev.action.id) ?? new MicBinding(settings);
    binding.update(settings); this.bindings.set(ev.action.id, binding);
    const context: KeyContext = { device: ev.action.device.id, handle: ev.action, renderer: new KeyRenderer(ev.action, history.calls), binding, settings, valid: migrated.valid };
    this.contexts.set(ev.action.id, context);
    this.render(context);
    if (migrated.changed) void context.handle.setSettings(settings).catch(() => {});
  }
  override onWillDisappear(ev: WillDisappearEvent<MicSettings>): void { this.remove(ev.action.id); }
  private remove(id: string): void {
    this.contexts.get(id)?.renderer.dispose(); this.contexts.delete(id); this.queue.cancelOwner(id);
    const history = this.callHistory.get(id);
    if (history) { clearTimeout(history.expiry); history.expiry = setTimeout(() => this.callHistory.delete(id), 1001); history.expiry.unref(); }
    if (this.inspector?.context === id) this.inspector = undefined;
  }
  override onDidReceiveSettings(ev: DidReceiveSettingsEvent<MicSettings>): void {
    const context = this.contexts.get(ev.action.id); if (!context) return;
    const migrated = migrateSettings(ev.payload.settings);
    if (JSON.stringify(context.settings) !== JSON.stringify(migrated.settings)) this.queue.cancelOwner(ev.action.id);
    context.settings = migrated.settings; context.valid = migrated.valid; context.error = undefined;
    context.binding.update(context.settings); this.render(context); this.sendOptions();
    if (migrated.changed) void context.handle.setSettings(context.settings).catch(() => {});
  }
  override onKeyDown(ev: KeyDownEvent<MicSettings>): void {
    const context = this.contexts.get(ev.action.id); if (!context) return;
    const snapshot = this.connection.store.snapshot;
    if (!context.valid) { this.fail(context, this.t("invalidSettings")); return; }
    if (!this.connection.ready || !snapshot || !this.connection.capabilities.includes("mic")) { this.fail(context, this.t(this.connection.status === "disabled" ? "disabled" : "disconnected")); return; }
    const binding = context.binding.resolve(snapshot);
    if (binding.status !== "bound") { this.fail(context, this.t(binding.status)); return; }
    context.error = undefined;
    const channelId = binding.channel.id, mode = context.settings.mode;
    void this.queue.enqueue(`mic/${channelId}`, ev.action.id, current => {
      if (!current.state.channels.some(c => c.id === channelId)) throw new ConnectionError("CHANNEL_NOT_FOUND");
      return mode === "toggle" ? { command: "toggleChannel", args: { channelId } } : { command: "setChannelOn", args: { channelId, on: mode === "on" } };
    }).catch((error: unknown) => {
      if (this.contexts.get(ev.action.id) !== context) return;
      const code = error instanceof ConnectionError ? error.code : "COMMAND_FAILED";
      this.fail(context, this.t(code === "CHANNEL_NOT_FOUND" ? "missing" : code === "INPUT_EXPIRED" || code === "INPUT_BUSY" ? "delayed" : "failure"));
    });
  }
  // No onKeyUp handler: releasing the key never issues a second command.
  private fail(context: KeyContext, message: string): void {
    context.error = message; context.renderer.alert(); this.sendOptions();
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
    for (const context of this.contexts.values()) this.render(context);
    this.sendOptions();
  }
  private render(context: KeyContext): void {
    let lamp: Lamp = "disconnected", title = this.t("disconnected"), state: 0 | 1 = 0, audioStopped = false, isName = false;
    const snapshot = this.connection.store.snapshot;
    if (this.connection.ready && snapshot) {
      const binding = context.binding.resolve(snapshot);
      if (!context.valid) { lamp = "missing"; title = this.t("invalidSettings"); }
      else if (binding.status !== "bound") { lamp = binding.status; title = this.t(binding.status); }
      else {
        const channel = binding.channel;
        state = channel.on ? 1 : 0;
        lamp = snapshot.state.muteGroups.mic && channel.muteGroup ? channel.on ? "muted-on" : "muted-off" : channel.on ? "on" : "off";
        title = context.settings.shortTitle || channel.name;
        isName = true;
        audioStopped = !snapshot.state.audio.running;
        if (binding.renamedOrigin !== undefined) {
          context.settings = { ...context.settings, channelName: binding.renamedOrigin };
          void context.handle.setSettings(context.settings).catch(() => {});
        }
      }
    } else if (this.connection.status === "disabled" || this.connection.status === "version" || this.connection.status === "checking" || this.connection.status === "connecting") {
      lamp = this.connection.status === "connecting" ? "checking" : this.connection.status;
      title = this.t(lamp);
    }
    context.renderer.render({ state, image: keyImage(lamp, this.language, audioStopped), title: isName ? displayName(title) : statusTitle(title) });
  }
  override onPropertyInspectorDidAppear(ev: PropertyInspectorDidAppearEvent<MicSettings>): void {
    if (this.contexts.has(ev.action.id)) this.inspector = { context: ev.action.id, requestId: "", sequence: 0 };
  }
  override onPropertyInspectorDidDisappear(ev: PropertyInspectorDidDisappearEvent<MicSettings>): void {
    if (this.inspector?.context === ev.action.id) this.inspector = undefined;
  }
  override onSendToPlugin(ev: SendToPluginEvent<JsonValue, MicSettings>): void {
    if (!isObject(ev.payload) || ev.payload.op !== "getOptions" || typeof ev.payload.requestId !== "string" || ev.payload.requestId.length > 64
      || !this.inspector || ev.action.id !== this.inspector.context || streamDeck.ui.action?.id !== ev.action.id) return;
    this.inspector.requestId = ev.payload.requestId; this.sendOptions();
  }
  private sendOptions(): void {
    const pi = this.inspector;
    if (!pi || !pi.requestId || streamDeck.ui.action?.id !== pi.context) return;
    const context = this.contexts.get(pi.context); if (!context) return;
    const snapshot = this.connection.store.snapshot, ready = this.connection.ready && !!snapshot;
    // This is a PI DTO, never discovery or a raw wire snapshot. Sequence also orders session changes.
    void streamDeck.ui.sendToPropertyInspector({
      op: "options", context: pi.context, requestId: pi.requestId, sequence: ++pi.sequence,
      connection: this.connection.status, language: this.language, settings: context.settings,
      message: context.error ?? this.t(ready ? "connected" : this.connection.status === "disabled" ? "disabledHelp" : "offlineHelp"),
      ...(ready ? {
        instanceId: snapshot.instanceId, sessionId: snapshot.sessionId, revision: snapshot.revision,
        channels: snapshot.state.channels.map(c => ({ id: c.id, name: c.name }))
      } : {})
    }).catch(() => {});
  }
}
