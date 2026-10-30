import { action } from "@elgato/streamdeck";
import { ConnectionError, type LiveMixConnection } from "../livemix/connection.js";
import type { CommandQueue } from "../livemix/command-queue.js";
import type { Snapshot } from "../livemix/protocol.js";
import { fxMuteCause } from "../livemix/muted.js";
import { displayName, keyImage } from "../ui/key-renderer.js";
import type { Language } from "../ui/i18n.js";
import { LiveMixAction, type ActionContext } from "./base.js";

@action({ UUID: "com.gomtwigim.livemix.fx" })
export class FxAction extends LiveMixAction {
  constructor(c: LiveMixConnection, q: CommandQueue, l: Language) { super(c, q, l, "fx", "fxOn"); }
  protected override pressKey(c: ActionContext, s: Snapshot): Promise<void> {
    const fxId = this.fx(c, s).id, mode = c.settings.mode;
    return this.queue.enqueue(`fx/${fxId}`, c.id, current => {
      if (!current.state.fx.some(fx => fx.id === fxId)) throw new ConnectionError("FX_NOT_FOUND");
      return mode === "toggle" ? { command: "toggleFx", args: { fxId } } : { command: "setFxOn", args: { fxId, on: mode === "on" } };
    });
  }
  protected override draw(c: ActionContext, s: Snapshot): void {
    const fx = this.fx(c, s);
    const cause = fxMuteCause(s.state, fx), lamp = cause ? (fx.on ? "muted-on" : "muted-off") : fx.on ? "on" : "off";
    c.key!.render({ state: fx.on ? 1 : 0, image: keyImage(lamp, this.language, !s.state.audio.running, false, "fx", cause ?? "group"),
      title: displayName(c.settings.shortTitle || fx.name) });
  }
}
