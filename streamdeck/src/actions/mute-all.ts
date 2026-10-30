import type { LiveMixConnection } from "../livemix/connection.js";
import type { CommandQueue } from "../livemix/command-queue.js";
import type { Snapshot } from "../livemix/protocol.js";
import { actionImage, displayName } from "../ui/key-renderer.js";
import type { Language } from "../ui/i18n.js";
import { LiveMixAction, type ActionContext } from "./base.js";

export class MuteAllAction extends LiveMixAction {
  constructor(c: LiveMixConnection, q: CommandQueue, l: Language, private readonly group: "mic" | "fx") { super(c, q, l, `${group}-mute-all`, "muteAll"); }
  protected override pressKey(c: ActionContext): Promise<void> {
    const group = this.group, mode = c.settings.mode;
    return this.queue.enqueue(`mute-all/${group}`, c.id, () => mode === "toggle"
      ? { command: "toggleMuteAll", args: { group } } : { command: "setMuteAll", args: { group, muted: mode === "mute" } });
  }
  protected override draw(c: ActionContext, s: Snapshot): void {
    const muted = s.state.muteAll[this.group], count = (this.group === "mic" ? s.state.channels : s.state.fx).length;
    c.key!.render({ state: muted ? 1 : 0, image: actionImage(muted ? "mute-every-muted" : "mute-every-clear", this.language, count, 0, !s.state.audio.running, this.group),
      title: displayName(c.settings.shortTitle || this.t(this.group === "mic" ? "micMuteAll" : "fxMuteAll")) });
  }
}
