import { action } from "@elgato/streamdeck";
import type { LiveMixConnection } from "../livemix/connection.js";
import type { CommandQueue } from "../livemix/command-queue.js";
import type { Language } from "../ui/i18n.js";
import { MuteAllAction } from "./mute-all.js";

@action({ UUID: "com.gomtwigim.livemix.fx-mute-all" })
export class FxMuteAllAction extends MuteAllAction {
  constructor(c: LiveMixConnection, q: CommandQueue, l: Language) { super(c, q, l, "fx"); }
}
