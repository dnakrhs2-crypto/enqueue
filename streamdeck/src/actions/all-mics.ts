import { action } from "@elgato/streamdeck";
import { ConnectionError, type LiveMixConnection } from "../livemix/connection.js";
import type { CommandQueue } from "../livemix/command-queue.js";
import type { Snapshot } from "../livemix/protocol.js";
import { actionImage, displayName } from "../ui/key-renderer.js";
import type { Language } from "../ui/i18n.js";
import { LiveMixAction, type ActionContext } from "./base.js";

@action({ UUID: "com.gomtwigim.livemix.all-mics" })
export class AllMicrophonesAction extends LiveMixAction {
  constructor(c: LiveMixConnection, q: CommandQueue, l: Language) { super(c, q, l, "all-mics", "mic"); }
  protected override pressKey(c: ActionContext): Promise<void> {
    const mode = c.settings.mode;
    return this.queue.enqueue("all-mics", c.id, current => {
      if (!current.state.channels.length) throw new ConnectionError("NO_CHANNELS");
      return { command: "setAllChannelsOn", args: { on: mode === "toggle" ? !current.state.channels.some(ch => ch.on) : mode === "on" } };
    }, true, 2);
  }
  protected override draw(c: ActionContext, s: Snapshot): void {
    const total = s.state.channels.length, count = s.state.channels.filter(ch => ch.on).length;
    if (!total) throw new ConnectionError("NO_CHANNELS");
    c.key!.render({ state: count === total ? 1 : 0, image: actionImage(count === total ? "all-on" : count ? "mixed" : "all-off", this.language, count, total, !s.state.audio.running),
      title: displayName(c.settings.shortTitle || this.t("allMics")) });
  }
}
