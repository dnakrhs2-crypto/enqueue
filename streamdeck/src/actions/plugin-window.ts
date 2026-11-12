import { action } from "@elgato/streamdeck";
import { ConnectionError, type LiveMixConnection } from "../livemix/connection.js";
import type { CommandQueue } from "../livemix/command-queue.js";
import type { Snapshot } from "../livemix/protocol.js";
import { displayName, windowImage } from "../ui/key-renderer.js";
import type { Language } from "../ui/i18n.js";
import { LiveMixAction, type ActionContext } from "./base.js";

function requireGroup(s: Snapshot, index: number, channelId?: string): void {
  const channels = channelId === undefined ? s.state.channels : s.state.channels.filter(ch => ch.id === channelId);
  if (channelId !== undefined && !channels.length) throw new ConnectionError("CHANNEL_NOT_FOUND");
  if (!channels.some(ch => ch.pluginGroups.some(g => g.index === index))) throw new ConnectionError("GROUP_NOT_FOUND");
}

@action({ UUID: "com.gomtwigim.livemix.plugin-window" })
export class PluginWindowAction extends LiveMixAction {
  constructor(c: LiveMixConnection, q: CommandQueue, l: Language) { super(c, q, l, "plugin-window", "pluginGroupWindows"); }
  protected override pressKey(c: ActionContext, s: Snapshot): Promise<void> {
    const channelId = c.settings.windowTarget === "mic" ? this.channel(c, s).id : undefined;
    const index = c.settings.groupIndex, mode = c.settings.mode;
    return this.queue.enqueue(`plugin-window/${channelId ?? "all"}/${index}`, c.id, current => {
      requireGroup(current, index, channelId);
      const args = { index, ...(channelId !== undefined ? { channelId } : {}) };
      return mode === "toggle" ? { command: "togglePluginGroupWindows", args }
        : { command: "setPluginGroupWindows", args: { ...args, open: mode === "open" } };
    });
  }
  protected override draw(c: ActionContext, s: Snapshot): void {
    const channel = c.settings.windowTarget === "mic" ? this.channel(c, s) : undefined, index = c.settings.groupIndex;
    requireGroup(s, index, channel?.id);
    const mode = c.settings.mode === "open" || c.settings.mode === "close" ? c.settings.mode : "toggle";
    c.key!.render({ state: 0, image: windowImage(mode, !channel, index, this.language, !s.state.audio.running),
      title: displayName(c.settings.shortTitle || `${channel?.name ?? this.t("allMicsShort")} · ${this.t("group")} ${index}`) });
  }
}
