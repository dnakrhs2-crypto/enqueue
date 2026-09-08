import { action } from "@elgato/streamdeck";
import type { LiveMixConnection } from "../livemix/connection.js";
import type { CommandQueue } from "../livemix/command-queue.js";
import type { Language } from "../ui/i18n.js";
import { MuteGroupAction } from "./mute-group.js";

@action({ UUID: "com.gomtwigim.livemix.fx-mute-group" })
export class FxMuteGroupAction extends MuteGroupAction {
  constructor(c: LiveMixConnection, q: CommandQueue, l: Language) { super(c, q, l, "fx"); }
}
