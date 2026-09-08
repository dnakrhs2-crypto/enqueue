// real_livemix_check.mjs — run the built plugin under the fake Stream Deck host against the REAL LiveMix
// (external control ON in the LiveMix settings, real %APPDATA%). Checks: registerPlugin, willAppear renders the
// current state, keyDown toggles the first mic in LiveMix (seen by an independent TCP observer), the key follows,
// then toggles it back. usage (from streamdeck/): node tools/real_livemix_check.mjs
import { readFile } from "node:fs/promises";
import { resolve } from "node:path";
import net from "node:net";
import { FakeHost } from "../tests/fake-host.mjs";

const appdata = process.env.APPDATA;
const discovery = JSON.parse(await readFile(resolve(appdata, "LiveMix/control/discovery.json"), "utf8"));
if (discovery.state !== "ready") { console.error("LiveMix external control is not ready:", discovery.state); process.exit(2); }

// independent observer on the control port
const lines = [];
let buffer = "";
const observer = net.createConnection({ host: discovery.host, port: discovery.port });
observer.on("data", chunk => { buffer += chunk.toString("utf8"); let i; while ((i = buffer.indexOf("\n")) >= 0) { lines.push(JSON.parse(buffer.slice(0, i))); buffer = buffer.slice(i + 1); } });
await new Promise(r => observer.once("connect", r));
observer.write(JSON.stringify({ v: 1, type: "hello", id: "1", supportedVersions: [1], token: discovery.token, client: { name: "real-check-observer", version: "0" } }) + "\n");
const waitObserver = (pred, ms = 6000) => new Promise((res, rej) => { const t0 = Date.now(); const tick = () => { const m = lines.find(pred); if (m) return res(m); if (Date.now() - t0 > ms) return rej(new Error("observer timeout")); setTimeout(tick, 50); }; tick(); });
const waitHost = (host, pred, ms = 8000) => new Promise((res, rej) => { const t0 = Date.now(); const tick = () => { const m = host.records.find(pred); if (m) return res(m); if (Date.now() - t0 > ms) return rej(new Error("host timeout; errors=" + host.errors.map(String).join("; ") + " output=" + host.output.slice(-400))); setTimeout(tick, 50); }; tick(); });
const state = await waitObserver(m => m.type === "state");
const mic = state.state.channels[0];
if (!mic) { console.error("no mic channel in the current session"); process.exit(2); }
console.log("observer: session", JSON.stringify(state.state.session.name), "| first mic:", mic.name, "on =", mic.on);

const host = new FakeHost();
await host.start(appdata, "ko");
host.appear("key-real", { settingsVersion: 1, channelId: mic.id, channelName: mic.name, mode: "toggle", nameFallback: true });
const first = await waitHost(host, r => r.event === "setState" && r.context === "key-real");
console.log("key state after willAppear:", first.payload.state, "(expected", mic.on ? 1 : 0 + ")");

const before = mic.on;
const seenBefore = lines.length, recBefore = host.records.length;
host.keyDown("key-real"); host.keyUp("key-real");
const delta = await waitObserver(m => lines.indexOf(m) >= seenBefore && (m.type === "stateDelta" || m.type === "state") && JSON.stringify(m).includes(mic.id) && JSON.stringify(m).includes(`"on":${!before}`));
console.log("LiveMix changed:", mic.name, "on =", !before, "(revision", delta.revision + ")");
const after = await waitHost(host, r => host.records.indexOf(r) >= recBefore && r.event === "setState" && r.context === "key-real" && r.payload.state === (!before ? 1 : 0));
console.log("key followed: state", after.payload.state);

// restore
const seen2 = lines.length;
host.keyDown("key-real"); host.keyUp("key-real");
await waitObserver(m => lines.indexOf(m) >= seen2 && (m.type === "stateDelta" || m.type === "state") && JSON.stringify(m).includes(mic.id) && JSON.stringify(m).includes(`"on":${before}`));
console.log("restored:", mic.name, "on =", before);
console.log("SDK output ok?", host.errors.length === 0, "| records:", host.records.length, "| token leaked to logs?", host.output.includes(discovery.token));
await host.stop?.();
observer.end();
console.log("REAL_CHECK_OK");
process.exit(0);
