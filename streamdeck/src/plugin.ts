import streamDeck from "@elgato/streamdeck";
import { MicrophoneAction } from "./actions/mic.js";
import { LiveMixConnection } from "./livemix/connection.js";
import { CommandQueue } from "./livemix/command-queue.js";
import { languageOf } from "./ui/i18n.js";

streamDeck.logger.setLevel("info");
const connection = new LiveMixConnection();
const queue = new CommandQueue(connection);
streamDeck.actions.registerAction(new MicrophoneAction(connection, queue, languageOf(streamDeck.info.application.language)));
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
