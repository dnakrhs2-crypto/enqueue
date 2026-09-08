import { performance } from "node:perf_hooks";
import { action } from "@elgato/streamdeck";
import type { LiveMixConnection } from "../livemix/connection.js";
import type { CommandQueue } from "../livemix/command-queue.js";
import type { Snapshot } from "../livemix/protocol.js";
import { actionImage, displayName, statusTitle } from "../ui/key-renderer.js";
import type { Language } from "../ui/i18n.js";
import { LiveMixAction, type ActionContext } from "./base.js";

@action({ UUID: "com.gomtwigim.livemix.status" })
export class StatusAction extends LiveMixAction {
  private readonly requests = new Map<string, number>();
  constructor(c: LiveMixConnection, q: CommandQueue, l: Language) { super(c, q, l, "status"); }
  protected override pressKey(c: ActionContext): Promise<void> | void {
    const now = performance.now();
    if (now - (this.requests.get(c.id) ?? -Infinity) < 2000) return;
    this.requests.set(c.id, now);
    const timer = setTimeout(() => { if (this.requests.get(c.id) === now) this.requests.delete(c.id); }, 2001); timer.unref();
    return this.connection.command({ command: "requestState", args: {} }).then(() => {});
  }
  protected override draw(c: ActionContext, s: Snapshot): void {
    const display = c.settings.display, stopped = display === "audio" && !s.state.audio.running;
    let title = this.t(display === "audio" ? stopped ? "audioStopped" : "audioRunning" : "connected");
    if (display === "session") title = displayName(s.state.session.name) + (s.state.session.dirty ? " *" : "");
    c.key!.render({ state: stopped ? 0 : 1, image: actionImage(stopped ? "status-off" : "status-on", this.language, 0, 0, !s.state.audio.running),
      title: display === "session" ? title : statusTitle(title) });
  }
}
