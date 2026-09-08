// Generates standalone HTML compositions. Claude exports these with Chromium at DPR 1.
import { mkdir, writeFile } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import { resolve } from "node:path";
import { brandSvg, micSvg, actionSvg, sendSvg, svgDocument, palette, xml } from "../src/ui/artwork.ts";
import { locales } from "./locales.mjs";
const out = fileURLToPath(new URL("../../docs/marketplace/livemix-streamdeck/", import.meta.url)), en = locales.en;
await mkdir(out, { recursive: true });
const css = `
*{box-sizing:border-box}html,body{margin:0;width:1920px;height:960px;overflow:hidden}
body{font-family:"Segoe UI",Arial,sans-serif;color:#F1F2F4;background:#15171B;-webkit-font-smoothing:antialiased}
.canvas{position:relative;width:1920px;height:960px;overflow:hidden;background:radial-gradient(ellipse at 90% 85%,#243348 0,transparent 45%),#15171B}
.canvas:before{content:"";position:absolute;left:842px;top:142px;bottom:134px;width:1px;background:#2F343C}
header{position:absolute;left:96px;right:96px;top:54px;display:flex;align-items:center;gap:18px}
header svg{width:58px;height:58px;border-radius:12px}header strong{font-size:35px;font-weight:650;letter-spacing:-1px}
.eyebrow{font-size:20px;font-weight:600;letter-spacing:4px;text-transform:uppercase;color:#98A0AB}
header .eyebrow{margin-left:auto}h1,h2,p{margin:0}
.copy{position:absolute;left:96px;top:188px;width:685px}
h1{font-size:82px;line-height:1.07;font-weight:650;letter-spacing:-3.5px}
.copy p{font-size:28px;line-height:1.55;color:#98A0AB;margin-top:30px;max-width:640px}
.hero .copy{top:220px}.hero h1{font-size:132px;letter-spacing:-7px}
.tagline{font-size:52px;line-height:1.17;font-weight:550;letter-spacing:-1.5px;margin-top:28px}
.hero .copy p{font-size:27px;max-width:590px}
.pills{display:flex;gap:12px;margin-top:38px}.pill{border:1px solid #3A3F47;border-radius:999px;padding:10px 18px;font-size:19px;color:#F1F2F4}
footer{position:absolute;left:96px;right:96px;bottom:34px;border-top:1px solid #2F343C;padding-top:20px;display:flex;justify-content:space-between;align-items:center;color:#98A0AB;font-size:19px}
.deck{position:absolute;background:linear-gradient(140deg,#343A43,#22262D 45%,#1B1E24);border:2px solid #4B525E;border-radius:38px;padding:28px 30px 32px;box-shadow:0 32px 64px #0009,inset 0 0 0 5px #1B1E24}
.deck-rail{height:34px;text-align:center;font-size:14px;letter-spacing:4px;color:#98A0AB}
.keys{display:grid;grid-template-columns:repeat(5,1fr);gap:15px}
.key{aspect-ratio:1;background:#111317;border:4px solid #101216;border-radius:15px;box-shadow:0 3px 0 #08090B,0 0 0 1px #3A3F47;overflow:hidden}
.key svg{display:block;width:100%;height:100%}.blank{background:#171A20}
.hero .deck{left:947px;top:278px;width:875px}
.standard-deck{left:947px;top:278px;width:875px}
.app-window{position:absolute;background:#1F2228;border:2px solid #3A3F47;border-radius:20px;overflow:hidden;box-shadow:0 22px 46px #0006;width:764px;height:396px;left:1002px;top:155px}
.app-top{height:54px;background:#1B1E24;padding:10px 18px;display:flex;align-items:center;gap:9px;font-size:19px}
.app-top svg{width:30px;height:30px;border-radius:7px}.window-buttons{margin-left:auto;display:flex;gap:17px}.window-buttons i{width:10px;height:10px;border:1px solid #98A0AB}
.app-toolbar{height:48px;border-bottom:1px solid #2F343C;display:flex;align-items:center;gap:14px;padding:0 22px}.line{height:7px;background:#3A3F47;border-radius:6px;width:64px}.line.short{width:32px}
.app-cards{display:flex;gap:14px;padding:18px}.app-card{flex:1;background:#272B32;border:1px solid #3A3F47;border-radius:12px;padding:17px}
.app-card .line{margin:0 0 18px}.app-lamp{height:30px;background:#1B1E24;border-radius:8px;display:flex;align-items:center;padding:8px;gap:10px;margin:18px 0}
.app-lamp i{width:12px;height:12px;border-radius:50%;background:#35D07F}.app-lamp.off i{background:#3A3F47}
.app-slot{height:26px;margin:8px 0;background:#1F2228;border:1px solid #3A3F47;border-radius:5px}
.app-fader{height:5px;background:#3A3F47;position:relative;margin:22px 0}.app-fader:after{content:"";position:absolute;left:35%;top:-4px;width:8px;height:13px;border-radius:3px;background:#4C8DFF}
.examples{position:absolute;left:96px;top:585px;width:686px;display:flex;gap:24px}
.example{width:190px}.example .key{width:156px;height:156px}.example p{font-size:22px;margin-top:17px;color:#98A0AB}
.notes{margin-top:40px;display:grid;gap:22px}.note{display:grid;grid-template-columns:10px 1fr;gap:18px;align-items:start;font-size:24px;line-height:1.35}
.note i{width:8px;height:34px;border-radius:4px;background:#35D07F}.note b{display:block;font-weight:600}.note span{display:block;margin-top:5px;color:#98A0AB;font-size:22px}
.groups .copy{top:185px}.groups .copy p{max-width:610px}.groups .notes{margin-top:35px}
.plus{left:963px;top:200px;width:802px;padding:26px 30px 28px;border-radius:38px}
.plus .keys{grid-template-columns:repeat(4,1fr);gap:17px}
.touch-strip{display:flex;border:3px solid #101216;background:#15171B;margin-top:22px;border-radius:9px;overflow:hidden}
.touch-strip svg{display:block;width:25%;height:auto;border-right:1px solid #3A3F47}.touch-strip svg:last-child{border:0}
.dials{display:flex;justify-content:space-around;margin-top:21px}.dial{height:65px;width:65px;border-radius:50%;border:3px solid #444C57;background:radial-gradient(circle at 35% 25%,#59616A,#30363E 64%,#171A20);box-shadow:0 5px 10px #0008;position:relative}
.dial:after{content:"";position:absolute;left:28px;top:7px;width:3px;height:12px;background:#98A0AB;border-radius:2px}
.send .app-window{left:1085px;top:145px;width:640px;height:290px}
.send .copy{top:183px}.send h1{font-size:78px}.send .examples{top:595px}
`;
function keyFace(svg, title) {
  const lines = title.split("\n");
  const name = '<g text-anchor="middle" font-family="Segoe UI,Arial,sans-serif" font-size="14" font-weight="500" fill="' + palette.text + '">' +
    lines.map((line, i) => '<text x="72" y="' + (lines.length === 1 ? 134 : 119 + i * 17) + '">' + xml(line) + '</text>').join("") + '</g>';
  return '<div class="key">' + svg.replace("</svg>", name + "</svg>") + '</div>';
}
const m = (state, name) => keyFace(micSvg(state, en), name);
const a = (state, name, count = 0, total = 0, group = "mic") => keyFace(actionSvg(state, en, count, total, false, group), name);
const s = (badge) => keyFace(sendSvg(35, badge, en), "Host → Hall");
const empty = '<div class="key blank"></div>';
const basicKeys = () => [
  m("on", "Host"), m("on", "Guest"), m("off", "Spare"), a("mixed", "All mics", 2, 3), a("status-on", "Connected"),
  a("group-clear", "Mic group", 2), a("group-clear", "FX group", 1, 0, "fx"), a("plugin-on", "Host · G1", 1), s("+5"), s("−5")
];
const groupKeys = () => [
  m("muted-on", "Host"), m("off", "Guest"), m("muted-on", "Spare"), a("mixed", "All mics", 2, 3), a("status-on", "Connected"),
  a("group-muted", "Mic group", 2), a("group-clear", "FX group", 1, 0, "fx"), a("plugin-on", "Host · G1", 1), a("plugin-off", "Host · G2", 2)
];
const deck = (keys, cls = "standard-deck") => '<div class="deck ' + cls + '" aria-label="Illustrated Stream Deck with fifteen keys"><div class="deck-rail">STREAM DECK</div><div class="keys">' + [...keys, ...Array(15 - keys.length).fill(empty)].join("") + '</div></div>';
function appWindow() {
  const card = (off) => '<div class="app-card"><div class="line"></div><div class="app-lamp' + (off ? ' off' : '') + '"><i></i><div class="line short"></div></div><div class="app-slot"></div><div class="app-slot"></div><div class="app-fader"></div></div>';
  return '<div class="app-window" aria-label="LiveMix window silhouette, not a translated screenshot"><div class="app-top">' + brandSvg(30) + '<b>LiveMix</b><div class="window-buttons"><i></i><i></i><i></i></div></div><div class="app-toolbar"><div class="line short"></div><div class="line"></div><div class="line"></div></div><div class="app-cards">' + card(false) + card(false) + card(true) + '</div></div>';
}
const example = (key, caption) => '<div class="example">' + key + '<p>' + caption + '</p></div>';
const note = (title, body, color = palette.lampOn) => '<div class="note"><i style="background:' + color + '"></i><div><b>' + title + '</b><span>' + body + '</span></div></div>';
function strip(name, percent, mode = "Post") {
  // Same 200×100 rectangles and typography as layouts/fx-send.json.
  return '<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100" viewBox="0 0 200 100"><rect width="200" height="100" fill="#15171B"/><g font-family="Segoe UI,Arial,sans-serif" fill="#FFFFFF"><text x="4" y="18" font-size="15">' + name + '</text><text x="4" y="54" font-size="32">' + percent + '%</text><text x="160" y="48" text-anchor="middle" font-size="18">' + mode + '</text><rect x="4" y="66" width="192" height="10" rx="4" fill="#3A3F47"/><rect x="4" y="66" width="' + percent * 1.92 + '" height="10" rx="4" fill="#4C8DFF"/></g></svg>';
}
function plus() {
  const keys = [m("on", "Host"), m("on", "Guest"), a("group-clear", "Mic group", 2), a("group-clear", "FX group", 1, 0, "fx"), s("+5"), s("−5"), s("=50"), a("status-on", "Connected")];
  return '<div class="deck plus" aria-label="Illustrated Stream Deck + with eight keys, a touch strip and four dials"><div class="deck-rail">STREAM DECK +</div><div class="keys">' + keys.join("") + '</div><div class="touch-strip">' + strip("Host → Hall", 35) + strip("Guest → Hall", 20) + strip("Host → Echo", 10, "Pre") + strip("Guest → Echo", 15) + '</div><div class="dials">' + '<div class="dial"></div>'.repeat(4) + '</div></div>';
}
function page(title, cls, content, footer) {
  return '<!doctype html>\n<html lang="en"><head><meta charset="utf-8"><meta name="canvas" content="1920x960"><meta name="viewport" content="width=1920,initial-scale=1"><title>' + title + '</title><style>' + css + '</style></head><body><main class="canvas ' + cls + '"><header>' + brandSvg(58) + '<strong>LiveMix</strong><div class="eyebrow">For Stream Deck</div></header>' + content + '<footer><span>' + footer + '</span><span>Illustrated controls · LiveMix UI is Korean</span></footer></main></body></html>\n';
}
const compositions = {
  "app-icon": '<!doctype html>\n<html lang="en"><head><meta charset="utf-8"><meta name="canvas" content="288x288"><title>LiveMix</title><style>html,body{margin:0;width:288px;height:288px;overflow:hidden;background:#15171B}svg{display:block}</style></head><body>' + brandSvg(288) + '</body></html>\n',
  thumbnail: page("LiveMix — Microphones and FX at your fingertips", "hero",
    '<section class="copy"><h1>LiveMix</h1><div class="tagline">Microphones and FX<br>at your fingertips</div><p>Control your microphones, mute groups<br>and FX sends while you stream.</p><div class="pills"><span class="pill">Windows</span><span class="pill">Keys &amp; dials</span><span class="pill">Free plugin</span></div></section>' + appWindow() + deck(basicKeys(), ""),
    'LiveMix 0.6.0+ · Stream Deck 7.1+ · Dials require Stream Deck +'),
  "gallery-01-microphones": page("LiveMix — Microphones", "microphones",
    '<section class="copy"><div class="eyebrow" style="margin-bottom:22px">01 / Microphones</div><h1>Every microphone.<br>A clear state.</h1><p>Toggle one mic or all of them.<br>See ON, OFF and mixed states at a glance.</p></section>' + appWindow() + deck(basicKeys()) +
    '<div class="examples">' + example(m("on", "Host"), 'One mic on') + example(m("off", "Spare"), 'One mic off') + example(a("mixed", "All mics", 2, 3), 'Two of three on') + '</div>',
    'Microphone keys work with Stream Deck hardware and Stream Deck Mobile'),
  "gallery-02-groups": page("LiveMix — Mute and plugin groups", "groups",
    '<section class="copy"><div class="eyebrow" style="margin-bottom:22px">02 / Groups</div><h1>One press.<br>The right group.</h1><p>Mute assigned microphones or FX.<br>Switch numbered plugin groups independently.</p><div class="notes">' +
    note("Microphone mute group", "Red marks a group mute. The original ON/OFF stays visible.", palette.danger) +
    note("FX mute group", "A separate switch for assigned FX returns.") +
    note("Plugin groups 1–5", "Blue is ON. Red is OFF.", palette.accent) + '</div></section>' + appWindow() + deck(groupKeys()),
    'Assign members and create numbered plugin groups in LiveMix'),
  "gallery-03-send": page("LiveMix — FX sends", "send",
    '<section class="copy"><div class="eyebrow" style="margin-bottom:22px">03 / FX Sends</div><h1>A little more FX.<br>A little less effort.</h1><p>Step or set the send amount with keys.<br>Turn a dial for fine control; short press<br>and release to switch Pre / Post.</p></section>' + appWindow() + plus() +
    '<div class="examples">' + example(s("+5"), 'Increase') + example(s("−5"), 'Decrease') + example(s("=50"), 'Set to 50%') + '</div>',
    'FX Send ± works on keys and Mobile · FX Send Amount requires Stream Deck +')
};
for (const [name, html] of Object.entries(compositions)) await writeFile(resolve(out, name + ".html"), html);
console.log("Generated 5 self-contained Marketplace HTML compositions (288×288 and 1920×960).");
