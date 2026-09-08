import { performance } from "node:perf_hooks";
import { action, type DialDownEvent, type DialRotateEvent, type DialUpEvent } from "@elgato/streamdeck";
import { ConnectionError, type LiveMixConnection } from "../livemix/connection.js";
import type { CommandQueue } from "../livemix/command-queue.js";
import type { ActionSettings } from "../livemix/bindings.js";
import { sendTarget, type Snapshot } from "../livemix/protocol.js";
import type { Language } from "../ui/i18n.js";
import { LiveMixAction, type ActionContext } from "./base.js";

@action({ UUID: "com.gomtwigim.livemix.fx-send" })
export class FxSendAction extends LiveMixAction {
  constructor(c: LiveMixConnection, q: CommandQueue, l: Language) { super(c, q, l, "fx-send", "sends"); }
  override onDialRotate(ev: DialRotateEvent<ActionSettings>): void {
    const c = this.contexts.get(ev.action.id); if (!c?.feedback) return;
    if (c.press) c.press.rotated = true;
    this.run(c, s => {
      const channelId = this.channel(c, s).id, fxId = this.fx(c, s).id;
      return this.queue.rotate(this.target(s, channelId, fxId), c.id, channelId, fxId, ev.payload.ticks, c.settings.stepPercent);
    });
  }
  override onDialDown(ev: DialDownEvent<ActionSettings>): void {
    const c = this.contexts.get(ev.action.id), s = this.connection.store.snapshot;
    if (c?.feedback) c.press = { at: performance.now(), rotated: false, session: s && this.connection.ready ? `${s.instanceId}/${s.sessionId}` : "" };
  }
  override onDialUp(ev: DialUpEvent<ActionSettings>): void {
    const c = this.contexts.get(ev.action.id), press = c?.press;
    if (!c || !press) return;
    c.press = undefined;
    if (press.rotated || performance.now() - press.at > 600 || c.settings.pressMode === "none") return;
    this.run(c, s => {
      if (press.session !== `${s.instanceId}/${s.sessionId}`) throw new ConnectionError("STALE_INPUT");
      const channelId = this.channel(c, s).id, fxId = this.fx(c, s).id;
      return this.queue.enqueue(this.target(s, channelId, fxId), c.id, current => {
        const target = sendTarget(current, channelId, fxId);
        if (!target) throw new ConnectionError("SEND_NOT_FOUND");
        return { command: "setSend", args: { channelId, fxId, pre: !target.send.pre } };
      }, true); // A pre/post conflict is shown once; never retry a press as a new toggle.
    });
  }
  private target(s: Snapshot, channelId: string, fxId: string): string { return `send/${s.instanceId}/${s.sessionId}/${channelId}/${fxId}`; }
  protected override draw(c: ActionContext, s: Snapshot): void {
    const channel = this.channel(c, s), fx = this.fx(c, s), target = sendTarget(s, channel.id, fx.id)!;
    const muted = (channel.muteGroup && s.state.muteGroups.mic) || (fx.muteGroup && s.state.muteGroups.fx);
    const name = `${channel.name} → ${fx.name}`.replace(/[\u0000-\u001f\u007f-\u009f]/g, " ");
    const percent = Math.round(target.send.amount * 100);
    c.feedback!.render({ name, amount: `${percent}%`, mode: this.t(target.send.pre ? "pre" : "post"), level: { value: percent, enabled: true },
      status: { value: c.error ?? (muted ? this.t("muteApplied") : !s.state.audio.running ? this.t("audioStopped") : ""), font: { size: 13 } } });
  }
}
