import { mkdir, readFile, writeFile, copyFile } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import { resolve, dirname } from "node:path";
import { locales } from "./locales.mjs";
import { actions, triggers } from "./actions.mjs";
import { brandPng } from "./brand-png.mjs";
import { brandSvg, categorySvg, glyph, svgDocument, micSvg, actionSvg, sendSvg, encoderSvg, palette } from "../src/ui/artwork.ts";

const root = fileURLToPath(new URL("../", import.meta.url));
const plugin = resolve(root, "com.gomtwigim.livemix.sdPlugin");
const design = resolve(root, "design");
async function write(path, data) { await mkdir(dirname(path), { recursive: true }); await writeFile(path, data); }
async function asset(path, data) {
  const source = resolve(design, path);
  await write(source, data);
  const destination = resolve(plugin, "imgs", path);
  await mkdir(dirname(destination), { recursive: true });
  await copyFile(source, destination);
}
const en = locales.en;
const emptySend = (kind, size) => svgDocument(size, '<rect width="144" height="144" rx="12" fill="' + palette.background + '"/><g transform="translate(48 10) scale(2)">' + glyph(kind, palette.dimText) + '</g><text x="72" y="99" text-anchor="middle" font-family="Segoe UI,sans-serif" font-size="30" fill="' + palette.text + '">—</text>');
function states(id, size) {
  switch (id) {
    case "mic": return { off: micSvg("off", en, false, false, size), on: micSvg("on", en, false, false, size),
      "muted-on": micSvg("muted-on", en, false, false, size), "muted-off": micSvg("muted-off", en, false, false, size) };
    case "all-mics": return { off: actionSvg("all-off", en, 0, 3, false, "mic", size), on: actionSvg("all-on", en, 3, 3, false, "mic", size),
      mixed: actionSvg("mixed", en, 2, 3, false, "mic", size) };
    case "mic-mute-group":
    case "fx-mute-group": return Object.fromEntries([["off", "group-clear"], ["on", "group-muted"]].map(([name, state]) => [name, actionSvg(state, en, 2, 0, false, id.startsWith("fx") ? "fx" : "mic", size)]));
    case "plugin-group": return Object.fromEntries([["off", "plugin-off"], ["on", "plugin-on"]].flatMap(([name, state]) => [
      [name, actionSvg(state, en, 1, 0, false, "mic", size)],
      ...[1, 2, 3, 4, 5].map(index => ["group-" + index + "-" + name, actionSvg(state, en, index, 0, false, "mic", size)])
    ]));
    case "plugin-group-all": return Object.fromEntries([["off", "plugin-all-off", 0], ["on", "plugin-all-on", 3], ["mixed", "plugin-all-mixed", 2]].flatMap(([name, state, count]) => [
      [name, actionSvg(state, en, count, 3, false, "mic", size)],
      ...[1, 2, 3, 4, 5].map(index => ["group-" + index + "-" + name, actionSvg(state, en, count, 3, false, "mic", size, index)])
    ]));
    case "fx-send": return { off: emptySend(id, size), on: svgDocument(size, '<rect width="144" height="144" rx="12" fill="' + palette.background + '"/><g transform="translate(48 8) scale(2)">' + glyph(id, palette.accent) + '</g><text x="72" y="98" text-anchor="middle" font-family="Segoe UI,sans-serif" font-size="25" fill="' + palette.text + '">35%</text>') };
    case "fx-send-step": return { off: emptySend(id, size), on: sendSvg(35, "+5", en, false, false, size),
      increase: sendSvg(35, "+5", en, false, false, size), decrease: sendSvg(35, "−5", en, false, false, size),
      set: sendSvg(50, "=50", en, false, false, size), zero: sendSvg(0, "+5", en, false, false, size),
      muted: sendSvg(35, "+5", en, true, false, size) };
    case "status": return { off: micSvg("disconnected", en, false, false, size), on: actionSvg("status-on", en, 0, 0, false, "mic", size),
      "audio-stopped": actionSvg("status-off", en, 0, 0, true, "mic", size) };
  }
}
for (const scale of [1, 2]) {
  const suffix = scale === 2 ? "@2x" : "", size = 72 * scale;
  // Plugin preferences require PNG; the geometric PNG is generated from the same brand shapes.
  await write(resolve(design, "plugin" + suffix + ".svg"), brandSvg(256 * scale) + "\n");
  await asset("plugin" + suffix + ".png", brandPng(256 * scale));
  await asset("category" + suffix + ".svg", categorySvg(28 * scale) + "\n");
  for (const a of actions) {
    const directory = "actions/" + a.id + "/";
    await asset(directory + "icon" + suffix + ".svg", svgDocument(20 * scale, glyph(a.id), 24) + "\n");
    for (const [name, body] of Object.entries(states(a.id, size))) await asset(directory + name + suffix + ".svg", body + "\n");
    await asset(directory + "disconnected" + suffix + ".svg", micSvg("disconnected", en, false, a.id.startsWith("fx-send"), size) + "\n");
    if (a.id === "fx-send") await asset(directory + "encoder" + suffix + ".svg", encoderSvg(size) + "\n");
  }
  for (const lamp of ["disabled", "checking", "version", "missing", "duplicate"]) await asset("status/" + lamp + suffix + ".svg", micSvg(lamp, en, false, false, size) + "\n");
}
for (const size of [256, 512]) await write(resolve(design, "plugin-" + size + ".html"),
  '<!doctype html>\n<html lang="en"><meta charset="utf-8"><meta name="canvas" content="' + size + 'x' + size + '"><title>LiveMix plugin icon</title><style>html,body{margin:0;width:' + size + 'px;height:' + size + 'px;overflow:hidden}svg{display:block}</style>' + brandSvg(size) + '</html>\n');

for (const [language, strings] of Object.entries(locales)) await write(resolve(plugin, `${language}.json`), JSON.stringify({
  Name: "LiveMix", ...Object.fromEntries(actions.map(a => [`com.gomtwigim.livemix.${a.id}`, {
    Name: strings[a.name], Tooltip: strings[a.tooltip], States: a.states.map(state => ({ Name: strings[state] })),
    ...(a.id === "fx-send" ? { Encoder: { TriggerDescription: triggers(strings) } } : {})
  }])), Localization: strings
}, null, 2) + "\n");
const manifest = JSON.parse(await readFile(resolve(plugin, "manifest.json"), "utf8"));
const pkg = JSON.parse(await readFile(resolve(root, "package.json"), "utf8"));
manifest.Version = pkg.version + ".0";
manifest.Description = "Control LiveMix microphones, mute groups, plugin groups and FX sends on this Windows PC.";
manifest.Actions = actions.map(a => ({ UUID: `com.gomtwigim.livemix.${a.id}`, Name: locales.en[a.name], Tooltip: locales.en[a.tooltip],
  Icon: `imgs/actions/${a.id}/icon`, PropertyInspectorPath: "ui/inspector.html", Controllers: [a.id === "fx-send" ? "Encoder" : "Keypad"],
  DisableAutomaticStates: true, DisableCaching: true, UserTitleEnabled: false, SupportedInMultiActions: false, SupportedInKeyLogicActions: false,
  States: a.states.map((state, i) => ({ Name: locales.en[state], Image: `imgs/actions/${a.id}/${i ? "on" : "off"}`, TitleAlignment: "bottom", FontSize: 12 })),
  ...(a.id === "fx-send" ? { Encoder: { layout: "layouts/fx-send.json", Icon: "imgs/actions/fx-send/encoder", TriggerDescription: triggers(locales.en) } } : {}) }));
await write(resolve(plugin, "manifest.json"), JSON.stringify(manifest, null, 2) + "\n");
await write(resolve(root, "src/ui/strings.ts"), `// Generated by tools/generate-assets.mjs; edit tools/locales.mjs.\nexport const strings = ${JSON.stringify(locales, null, 2)} as const;\n`);
await write(resolve(plugin, "ui/strings.js"), `// Generated by tools/generate-assets.mjs.\nwindow.LiveMixStrings = ${JSON.stringify(locales, null, 2)};\n`);
console.log("Generated LiveMix release icons (1x/2x), design sources and ko/en strings.");
