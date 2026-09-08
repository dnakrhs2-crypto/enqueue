import streamDeck from "@elgato/streamdeck";
import { MicrophoneAction } from "./actions/mic.js";
import { AllMicrophonesAction } from "./actions/all-mics.js";
import { MicrophoneMuteGroupAction } from "./actions/mic-mute-group.js";
import { FxMuteGroupAction } from "./actions/fx-mute-group.js";
import { PluginGroupAction } from "./actions/plugin-group.js";
import { FxSendAction } from "./actions/fx-send.js";
import { StatusAction } from "./actions/status.js";
import { LiveMixConnection } from "./livemix/connection.js";
import { CommandQueue } from "./livemix/command-queue.js";
import { languageOf } from "./ui/i18n.js";

streamDeck.logger.setLevel("info");
const connection = new LiveMixConnection();
const queue = new CommandQueue(connection);
const language = languageOf(streamDeck.info.application.language);
for (const Action of [MicrophoneAction, AllMicrophonesAction, MicrophoneMuteGroupAction, FxMuteGroupAction, PluginGroupAction, FxSendAction, StatusAction]) {
  streamDeck.actions.registerAction(new Action(connection, queue, language));
}
let lastFailure = -Infinity;
connection.on("disconnect", () => {
  if (Date.now() - lastFailure >= 30000) { lastFailure = Date.now(); streamDeck.logger.info("LiveMix control connection closed; awaiting discovery."); }
});
function stop(): void { queue.dispose(); connection.stop(); }
process.once("SIGTERM", () => { stop(); process.exit(0); });
process.once("SIGINT", () => { stop(); process.exit(0); });
process.once("exit", stop);
await streamDeck.connect();
connection.start();
