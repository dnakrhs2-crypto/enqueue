import { action } from "@elgato/streamdeck";
import { ConnectionError, type LiveMixConnection } from "../livemix/connection.js";
import type { CommandQueue } from "../livemix/command-queue.js";
import type { Snapshot } from "../livemix/protocol.js";
import { displayName, keyImage } from "../ui/key-renderer.js";
import type { Language } from "../ui/i18n.js";
import { LiveMixAction, type ActionContext } from "./base.js";

@action({ UUID: "com.gomtwigim.livemix.aux" })
export class AuxAction extends LiveMixAction {
  constructor(c: LiveMixConnection, q: CommandQueue, l: Language) { super(c, q, l, "aux", "auxOn"); }
  protected override pressKey(c: ActionContext, s: Snapshot): Promise<void> {
    const auxId = this.aux(c, s).id, mode = c.settings.mode;
    return this.queue.enqueue(`aux/${auxId}`, c.id, current => {
      if (!current.state.auxes.some(aux => aux.id === auxId)) throw new ConnectionError("AUX_NOT_FOUND");
      return mode === "toggle" ? { command: "toggleAux", args: { auxId } } : { command: "setAuxOn", args: { auxId, on: mode === "on" } };
    });
  }
  protected override draw(c: ActionContext, s: Snapshot): void {
    const aux = this.aux(c, s);
    c.key!.render({ state: aux.on ? 1 : 0, image: keyImage(aux.on ? "on" : "off", this.language, !s.state.audio.running, false, "aux"),
      title: displayName(c.settings.shortTitle || aux.name) });
  }
}
