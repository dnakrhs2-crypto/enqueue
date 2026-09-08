import { action } from "@elgato/streamdeck";
import { ConnectionError, type LiveMixConnection } from "../livemix/connection.js";
import type { CommandQueue } from "../livemix/command-queue.js";
import { sendTarget, type Snapshot } from "../livemix/protocol.js";
import { displayName, keyImage, sendImage, type Lamp } from "../ui/key-renderer.js";
import type { Language } from "../ui/i18n.js";
import { LiveMixAction, type ActionContext } from "./base.js";

@action({ UUID: "com.gomtwigim.livemix.fx-send-step" })
export class FxSendStepAction extends LiveMixAction {
  constructor(c: LiveMixConnection, q: CommandQueue, l: Language) { super(c, q, l, "fx-send-step", "sends"); }
  protected override pressKey(c: ActionContext, s: Snapshot): Promise<void> {
    const channelId = this.channel(c, s).id, fxId = this.fx(c, s).id;
    const { mode, stepPercent, targetPercent } = c.settings;
    // Share the dial's target queue, including its ACK + canonical-state barrier.
    return this.queue.enqueue(`send/${s.instanceId}/${s.sessionId}/${channelId}/${fxId}`, c.id, current => {
      const target = sendTarget(current, channelId, fxId);
      if (!target) throw new ConnectionError("SEND_NOT_FOUND");
      const value = mode === "set" ? targetPercent / 100 : target.send.amount + (mode === "down" ? -1 : 1) * stepPercent / 100;
      return { command: "setSend", args: { channelId, fxId, amount: Math.min(1, Math.max(0, value)) } };
    }, true, 1); // Only an explicit revision conflict may recompute once.
  }
  protected override problemImage(lamp: Lamp): string { return keyImage(lamp, this.language, false, true); }
  protected override draw(c: ActionContext, s: Snapshot): void {
    const channel = this.channel(c, s), fx = this.fx(c, s), target = sendTarget(s, channel.id, fx.id)!;
    const { mode, stepPercent, targetPercent } = c.settings;
    const badge = mode === "set" ? `=${targetPercent}` : `${mode === "down" ? "−" : "+"}${stepPercent}`;
    const muted = (channel.muteGroup && s.state.muteGroups.mic) || (fx.muteGroup && s.state.muteGroups.fx);
    c.key!.render({ state: 0, image: sendImage(Math.round(target.send.amount * 100), badge, this.language, muted, !s.state.audio.running),
      title: displayName(`${channel.name} → ${fx.name}`) });
  }
}
