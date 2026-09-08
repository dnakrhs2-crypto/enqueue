import { mkdir, readFile, writeFile } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import { resolve, dirname } from "node:path";
import { deflateSync } from "node:zlib";
import { locales } from "./locales.mjs";
import { actions, triggers } from "./actions.mjs";

const root = fileURLToPath(new URL("../", import.meta.url));
const plugin = resolve(root, "com.gomtwigim.livemix.sdPlugin");
async function write(path, data) { await mkdir(dirname(path), { recursive: true }); await writeFile(path, data); }
function crc32(bytes) {
  let crc = 0xffffffff;
  for (const byte of bytes) { crc ^= byte; for (let n = 0; n < 8; n++) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0); }
  return (crc ^ 0xffffffff) >>> 0;
}
function chunk(type, bytes) {
  const name = Buffer.from(type), len = Buffer.alloc(4), crc = Buffer.alloc(4);
  len.writeUInt32BE(bytes.length); crc.writeUInt32BE(crc32(Buffer.concat([name, bytes])));
  return Buffer.concat([len, name, bytes, crc]);
}
// Pure Node PNG encoder: placeholder circles only; no native image library or downloaded artwork.
function png(size) {
  const header = Buffer.alloc(13); header.writeUInt32BE(size, 0); header.writeUInt32BE(size, 4); header[8] = 8; header[9] = 6;
  const pixels = Buffer.alloc((size * 4 + 1) * size);
  for (let y = 0; y < size; y++) for (let x = 0; x < size; x++) {
    const p = y * (size * 4 + 1) + 1 + x * 4;
    const lamp = Math.hypot(x - size / 2, y - size / 2) < size * 0.3;
    pixels.set(lamp ? [255, 90, 95, 255] : [21, 23, 27, 255], p);
  }
  return Buffer.concat([Buffer.from([137,80,78,71,13,10,26,10]), chunk("IHDR", header), chunk("IDAT", deflateSync(pixels)), chunk("IEND", Buffer.alloc(0))]);
}
const svg = (size, body) => `<svg xmlns="http://www.w3.org/2000/svg" width="${size}" height="${size}" viewBox="0 0 144 144">${body}</svg>\n`;
for (const scale of [1, 2]) {
  const suffix = scale === 2 ? "@2x" : "";
  await write(resolve(plugin, `imgs/plugin${suffix}.png`), png(256 * scale));
  for (const [path, size] of [["category", 28], ["actions/mic/icon", 20]]) {
    await write(resolve(plugin, `imgs/${path}${suffix}.svg`), svg(size * scale, '<g fill="none" stroke="#FFFFFF" stroke-width="12"><rect x="51" y="16" width="42" height="72" rx="21"/><path d="M34 68v6a38 38 0 0 0 76 0v-6M72 112v18M46 130h52"/></g>'));
  }
  // Both manifest states start offline to avoid an old green lamp before the first snapshot.
  for (const state of ["on", "off"]) await write(resolve(plugin, `imgs/actions/mic/${state}${suffix}.svg`), svg(72 * scale, '<rect width="144" height="144" rx="16" fill="#15171B"/><g fill="none" stroke="#AEB6C2" stroke-width="7"><path d="M30 26v28h25l12 12m47 52V90H89L77 78M52 38l-8 16m61 36l-9 16"/><path stroke="#FF5A5F" d="m70 43 10 10m-25 26 10 10"/></g>'));
  const shapes = {
    "all-mics": '<circle cx="30" cy="72" r="19"/><circle cx="72" cy="72" r="19"/><circle cx="114" cy="72" r="19"/>',
    "mic-mute-group": '<rect x="22" y="30" width="100" height="84" rx="12"/><path d="m25 32 94 80M72 45v28m-18 0a18 18 0 0 0 36 0"/>',
    "fx-mute-group": '<rect x="22" y="30" width="100" height="84" rx="12"/><path d="m25 32 94 80M47 91V53h24m-24 18h20m12-18 23 38m0-38L79 91"/>',
    "plugin-group": '<rect x="25" y="25" width="94" height="94" rx="14"/><path d="M56 57 72 43v58m-19 0h38"/>',
    "fx-send": '<circle cx="72" cy="72" r="44"/><path d="M72 72V32m28 53 17 3-2-18"/>',
    status: '<circle cx="72" cy="72" r="46"/><path d="M72 65v37m0-59v8"/>'
  };
  for (const a of actions.filter(a => a.id !== "mic")) {
    const body = `<g fill="none" stroke="#FFFFFF" stroke-width="8">${shapes[a.id]}</g>`;
    await write(resolve(plugin, `imgs/actions/${a.id}/icon${suffix}.svg`), svg(20 * scale, body));
    for (const state of ["off", "on"]) await write(resolve(plugin, `imgs/actions/${a.id}/${state}${suffix}.svg`), svg(72 * scale,
      `<rect width="144" height="144" rx="12" fill="#15171B"/>${body}<path d="m30 120 84-96" stroke="#FF5A5F" stroke-width="8"/>`));
    if (a.id === "fx-send") await write(resolve(plugin, `imgs/actions/fx-send/encoder${suffix}.svg`), svg(72 * scale, body));
  }
}
for (const [language, strings] of Object.entries(locales)) await write(resolve(plugin, `${language}.json`), JSON.stringify({
  Name: "LiveMix", ...Object.fromEntries(actions.map(a => [`com.gomtwigim.livemix.${a.id}`, {
    Name: strings[a.name], Tooltip: strings[a.tooltip], States: a.states.map(state => ({ Name: strings[state] })),
    ...(a.id === "fx-send" ? { Encoder: { TriggerDescription: triggers(strings) } } : {})
  }])), Localization: strings
}, null, 2) + "\n");
const manifest = JSON.parse(await readFile(resolve(plugin, "manifest.json"), "utf8"));
manifest.Description = "Control LiveMix microphones, mute groups, plugin groups and FX sends on this Windows PC.";
manifest.Actions = actions.map(a => ({ UUID: `com.gomtwigim.livemix.${a.id}`, Name: locales.en[a.name], Tooltip: locales.en[a.tooltip],
  Icon: `imgs/actions/${a.id}/icon`, PropertyInspectorPath: "ui/inspector.html", Controllers: [a.id === "fx-send" ? "Encoder" : "Keypad"],
  DisableAutomaticStates: true, DisableCaching: true, UserTitleEnabled: false, SupportedInMultiActions: false, SupportedInKeyLogicActions: false,
  States: a.states.map((state, i) => ({ Name: locales.en[state], Image: `imgs/actions/${a.id}/${i ? "on" : "off"}`, TitleAlignment: "bottom", FontSize: 12 })),
  ...(a.id === "fx-send" ? { Encoder: { layout: "layouts/fx-send.json", Icon: "imgs/actions/fx-send/encoder", TriggerDescription: triggers(locales.en) } } : {}) }));
await write(resolve(plugin, "manifest.json"), JSON.stringify(manifest, null, 2) + "\n");
await write(resolve(root, "src/ui/strings.ts"), `// Generated by tools/generate-assets.mjs; edit tools/locales.mjs.\nexport const strings = ${JSON.stringify(locales, null, 2)} as const;\n`);
await write(resolve(plugin, "ui/strings.js"), `// Generated by tools/generate-assets.mjs.\nwindow.LiveMixStrings = ${JSON.stringify(locales, null, 2)};\n`);
console.log("Generated placeholder icons (1x/2x) and ko/en strings.");
