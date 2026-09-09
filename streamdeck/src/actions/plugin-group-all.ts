import { action } from "@elgato/streamdeck";
import { ConnectionError, type LiveMixConnection } from "../livemix/connection.js";
import type { CommandQueue } from "../livemix/command-queue.js";
import type { Snapshot } from "../livemix/protocol.js";
import { actionImage, displayName } from "../ui/key-renderer.js";
import type { Language } from "../ui/i18n.js";
import { LiveMixAction, type ActionContext } from "./base.js";

@action({ UUID: "com.gomtwigim.livemix.plugin-group-all" })
export class PluginGroupEverywhereAction extends LiveMixAction {
  constructor(c: LiveMixConnection, q: CommandQueue, l: Language) { super(c, q, l, "plugin-group-all", "pluginGroupsEverywhere"); }
  protected override pressKey(c: ActionContext): Promise<void> {
    const index = c.settings.groupIndex, mode = c.settings.mode;
    return this.queue.enqueue(`plugin-group-all/${index}`, c.id, current => {
      const targets = current.state.channels.flatMap(ch => ch.pluginGroups.filter(g => g.index === index));
      if (!targets.length) throw new ConnectionError("GROUP_NOT_FOUND");
      return { command: "setPluginGroupOffEverywhere", args: { index, off: mode === "toggle" ? targets.some(g => !g.off) : mode === "off" } };
    }, true, 2);
  }
  protected override draw(c: ActionContext, s: Snapshot): void {
    const index = c.settings.groupIndex, targets = s.state.channels.flatMap(ch => ch.pluginGroups.filter(g => g.index === index));
    const total = targets.length, onCount = targets.filter(g => g.off === false).length;
    if (!total) throw new ConnectionError("GROUP_NOT_FOUND");
    c.key!.render({ state: onCount === total ? 1 : 0,
      image: actionImage(onCount === total ? "plugin-all-on" : onCount ? "plugin-all-mixed" : "plugin-all-off", this.language, onCount, total, !s.state.audio.running, "mic", index),
      title: displayName(c.settings.shortTitle || `${this.t("allMicsShort")} · ${this.t("group")} ${index}`) });
  }
}
