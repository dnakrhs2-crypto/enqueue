import assert from "node:assert/strict";
import { readFile, readdir } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import { resolve } from "node:path";
const root = fileURLToPath(new URL("../", import.meta.url));
const plugin = resolve(root, "com.gomtwigim.livemix.sdPlugin");
const read = path => readFile(resolve(plugin, path));
const manifest = JSON.parse(await read("manifest.json"));
const pkg = JSON.parse(await readFile(resolve(root, "package.json"), "utf8"));
assert.equal(manifest.Version, pkg.version + ".0", "Manifest and npm release versions");
const refs = [
  [manifest.Icon, 256, "png"], [manifest.CategoryIcon, 28, "svg"],
  ...manifest.Actions.flatMap(a => [[a.Icon, 20, "svg"], ...a.States.map(s => [s.Image, 72, "svg"]),
    ...(a.Encoder?.Icon ? [[a.Encoder.Icon, 72, "svg"]] : [])])
];
let checked = 0;
for (const [path, size, extension] of refs) for (const scale of [1, 2]) {
  const relative = path + (scale === 2 ? "@2x" : "") + "." + extension, data = await read(relative);
  if (extension === "png") {
    assert.deepEqual([...data.subarray(0, 8)], [137,80,78,71,13,10,26,10], relative + " PNG signature");
    assert.equal(data.readUInt32BE(16), size * scale, relative + " width");
    assert.equal(data.readUInt32BE(20), size * scale, relative + " height");
  } else {
    const svg = data.toString("utf8"), header = svg.match(/^<svg\b[^>]*>/)?.[0] || "";
    assert.ok(header.includes('width="' + size * scale + '"') && header.includes('height="' + size * scale + '"'), relative + " dimensions");
    assert.ok(header.includes('viewBox="0 0 ' + (size < 72 ? "24 24" : "144 144") + '"'), relative + " viewBox");
    assert.ok(!/<(?:script|image|foreignObject)\b|(?:href|src)=/i.test(svg), relative + " self contained SVG");
    if (size < 72) {
      const colors = [...svg.matchAll(/(?:fill|stroke)="([^"]+)"/g)].map(m => m[1]);
      assert.ok(colors.includes("#FFFFFF") && colors.every(c => c === "#FFFFFF" || c === "none"), relative + " white monochrome");
      assert.ok(!/opacity=/.test(svg), relative + " fully opaque strokes");
    }
  }
  assert.deepEqual(data, await readFile(resolve(root, "design", relative.replace(/^imgs\//, ""))), relative + " matches design source");
  checked++;
}
// Include state variants not directly referenced by the manifest.
const walk = async path => (await Promise.all((await readdir(path, { withFileTypes: true })).map(async e => e.isDirectory() ? walk(resolve(path, e.name)) : [resolve(path, e.name)]))).flat();
const files = await walk(resolve(plugin, "imgs"));
for (const file of files) {
  assert.deepEqual(await readFile(file), await readFile(resolve(root, "design", file.slice(resolve(plugin, "imgs").length + 1))), file + " reproducible source");
}
const en = JSON.parse(await read("en.json")), ko = JSON.parse(await read("ko.json"));
assert.deepEqual(Object.keys(en.Localization).sort(), Object.keys(ko.Localization).sort(), "Locale keys");
for (const locale of [en, ko]) {
  assert.ok(Object.values(locale.Localization).every(v => typeof v === "string" && v.trim()), "Complete locale values");
  for (const action of manifest.Actions) {
    const translated = locale[action.UUID];
    assert.ok(translated?.Name && translated.Tooltip, action.UUID + " translation");
    assert.equal(translated.States.length, action.States.length, action.UUID + " state translations");
    assert.ok(translated.States.every(s => s.Name), action.UUID + " state names");
    if (action.Encoder) for (const trigger of ["Push", "Rotate", "Touch", "LongTouch"]) assert.ok(translated.Encoder.TriggerDescription[trigger]);
  }
}
console.log("Verified " + refs.length + " manifest image references / " + checked + " files in both sizes; " + files.length + " packaged images match design sources; ko/en complete.");
