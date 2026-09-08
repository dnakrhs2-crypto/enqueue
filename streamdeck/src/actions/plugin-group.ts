import { action } from "@elgato/streamdeck";
import { ConnectionError, type LiveMixConnection } from "../livemix/connection.js";
import type { CommandQueue } from "../livemix/command-queue.js";
import type { Snapshot } from "../livemix/protocol.js";
import { actionImage, displayName } from "../ui/key-renderer.js";
import type { Language } from "../ui/i18n.js";
import { LiveMixAction, type ActionContext } from "./base.js";

@action({ UUID: "com.gomtwigim.livemix.plugin-group" })
export class PluginGroupAction extends LiveMixAction {
  constructor(c: LiveMixConnection, q: CommandQueue, l: Language) { super(c, q, l, "plugin-group", "pluginGroups"); }
  protected override pressKey(c: ActionContext, s: Snapshot): Promise<void> {
    const channelId = this.channel(c, s).id, index = c.settings.groupIndex, mode = c.settings.mode;
    return this.queue.enqueue(`plugin-group/${channelId}/${index}`, c.id, current => {
      const channel = current.state.channels.find(ch => ch.id === channelId);
      if (!channel) throw new ConnectionError("CHANNEL_NOT_FOUND");
      const group = channel.pluginGroups.find(g => g.index === index);
      if (!group) throw new ConnectionError("GROUP_NOT_FOUND");
      return { command: "setPluginGroupOff", args: { channelId, index, off: mode === "toggle" ? !group.off : mode === "off" } };
    }, true, 2);
  }
  protected override draw(c: ActionContext, s: Snapshot): void {
    const channel = this.channel(c, s), group = channel.pluginGroups.find(g => g.index === c.settings.groupIndex);
    if (!group) throw new ConnectionError("GROUP_NOT_FOUND");
    c.key!.render({ state: group.off ? 0 : 1, image: actionImage(group.off ? "plugin-off" : "plugin-on", this.language, group.index, 0, !s.state.audio.running),
      title: displayName(c.settings.shortTitle || `${channel.name} · ${this.t("group")} ${group.index}`) });
  }
}
