import type { LiveMixConnection } from "../livemix/connection.js";
import type { CommandQueue } from "../livemix/command-queue.js";
import type { Snapshot } from "../livemix/protocol.js";
import { actionImage, displayName } from "../ui/key-renderer.js";
import type { Language } from "../ui/i18n.js";
import { LiveMixAction, type ActionContext } from "./base.js";

export class MuteGroupAction extends LiveMixAction {
  constructor(c: LiveMixConnection, q: CommandQueue, l: Language, private readonly group: "mic" | "fx") { super(c, q, l, `${group}-mute-group`, "muteGroups"); }
  protected override pressKey(c: ActionContext): Promise<void> {
    const group = this.group, mode = c.settings.mode;
    return this.queue.enqueue(`mute-group/${group}`, c.id, () => mode === "toggle"
      ? { command: "toggleMuteGroup", args: { group } } : { command: "setMuteGroup", args: { group, muted: mode === "mute" } });
  }
  protected override draw(c: ActionContext, s: Snapshot): void {
    const muted = s.state.muteGroups[this.group], count = (this.group === "mic" ? s.state.channels : s.state.fx).filter(item => item.muteGroup).length;
    c.key!.render({ state: muted ? 1 : 0, image: actionImage(muted ? "group-muted" : "group-clear", this.language, count, 0, !s.state.audio.running),
      title: displayName(c.settings.shortTitle || this.t(this.group === "mic" ? "micMuteGroup" : "fxMuteGroup")) });
  }
}
