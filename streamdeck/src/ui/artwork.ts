/**
 * LiveMix's vector source, shared by runtime keys, generated assets and media.
 * Standalone so Node 24 can also import this file in the asset generators.
 * Coordinates: glyphs 24×24, keys/brand 144×144. Key titles belong below y=106.
 */
export const palette = {
  background: "#15171B", bar: "#1B1E24", card: "#1F2228", card2: "#272B32",
  line: "#2F343C", text: "#F1F2F4", dimText: "#98A0AB", accent: "#4C8DFF",
  lampOn: "#35D07F", lampOff: "#3A3F47", brand: "#E5302D", danger: "#FF5A5F",
  warning: "#FFB454", white: "#FFFFFF"
} as const;
export type IconKind = "mic" | "all-mics" | "mic-mute-group" | "fx-mute-group" | "plugin-group" | "plugin-group-all" | "fx-send" | "fx-send-step" | "status";
export type Lamp = "on" | "off" | "muted-on" | "muted-off" | "disconnected" | "disabled" | "checking" | "version" | "missing" | "duplicate";
export type ActionLamp = "all-on" | "all-off" | "mixed" | "group-muted" | "group-clear" | "plugin-on" | "plugin-off" | "plugin-all-on" | "plugin-all-off" | "plugin-all-mixed" | "status-on" | "status-off";
export type Labels = { [key: string]: string };
export const xml = (s: string): string => s.replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&apos;" })[c]!);
export const svgDocument = (size: number, body: string, viewBox = 144): string =>
  '<svg xmlns="http://www.w3.org/2000/svg" width="' + size + '" height="' + size + '" viewBox="0 0 ' + viewBox + ' ' + viewBox + '">' + body + '</svg>';
const rect = (x: number, y: number, w: number, h: number, r: number, color: string): string =>
  '<rect x="' + x + '" y="' + y + '" width="' + w + '" height="' + h + '" rx="' + r + '" fill="' + color + '"/>';
const text = (label: string, y: number, size = 16, color: string = palette.text, x = 72): string =>
  '<text x="' + x + '" y="' + y + '" font-size="' + size + '" fill="' + color + '">' + xml(label) + '</text>';
const stroke = (body: string, color: string, width = 1.8): string =>
  '<g fill="none" stroke="' + color + '" stroke-width="' + width + '" stroke-linecap="round" stroke-linejoin="round">' + body + '</g>';
const mic = '<rect x="9" y="2" width="6" height="12" rx="3"/><path d="M6 10v2a6 6 0 0 0 12 0v-2M12 18v4M9 22h6"/>';
const fx = '<path d="M5 17V7h6M5 12h5M14 7l6 10M20 7l-6 10"/>';
const brackets = '<path d="M6 2H3v20h3M18 2h3v20h-3"/>';
const digits = [
  "", "M10 9l2-2v10M10 17h4", "M9 9a3 3 0 0 1 6 0c0 2-6 5-6 8h6",
  "M9 7h6l-4 5h1a3 3 0 0 1 0 6H9", "M14 18V7l-6 7h9",
  "M15 7H9v5h3a3 3 0 0 1 0 6H9"
];
export function glyph(kind: IconKind, color: string = palette.white, index = 1): string {
  let body: string;
  switch (kind) {
    case "mic": body = mic; break;
    case "all-mics": body = [4, 12, 20].map(x => '<rect x="' + (x - 2) + '" y="4" width="4" height="9" rx="2"/><path d="M' + (x - 3) + ' 11v2a3 3 0 0 0 6 0v-2M' + x + ' 16v4M' + (x - 2) + ' 20h4"/>').join(""); break;
    case "mic-mute-group": body = brackets + '<g transform="translate(4 3) scale(.667 .75)">' + mic + '</g>'; break;
    case "fx-mute-group": body = brackets + '<g transform="translate(2 2) scale(.833)">' + fx + '</g>'; break;
    case "plugin-group": body = '<rect x="5" y="4" width="14" height="16" rx="3"/><path d="M2 8h3M2 16h3M19 8h3M19 16h3M9 1v3M15 1v3M9 20v3M15 20v3"/><path d="' + digits[Math.max(1, Math.min(5, index))] + '"/>'; break;
    case "plugin-group-all": body = '<path d="M7 2h11a3 3 0 0 1 3 3v13"/><rect x="4" y="5" width="14" height="16" rx="3"/><path d="M1 9h3M1 17h3M18 9h3M18 17h3M8 2v3M14 2v3"/><g transform="translate(-1 1)"><path d="' + digits[Math.max(1, Math.min(5, index))] + '"/></g>'; break;
    case "fx-send": body = '<path d="M5 16a8 8 0 1 1 14 0M12 12l4-5M9 21h6"/><circle cx="12" cy="12" r="1"/>'; break;
    case "fx-send-step": body = '<path d="M3 7h7M6.5 3.5v7M15 7h6M4 15h16v6H4zM8 16v4M12 16v4"/>'; break;
    case "status": body = '<circle cx="12" cy="12" r="9"/><path d="M12 3v8M6 10a6 6 0 1 0 12 0"/>'; break;
  }
  return stroke(body, color);
}
const placed = (body: string, x: number, y: number, size: number): string =>
  '<g transform="translate(' + x + ' ' + y + ') scale(' + size / 24 + ')">' + body + '</g>';
const pause = (stopped: boolean, labels: Labels): string => stopped
  ? '<path d="M123 9v12m8-12v12" stroke="' + palette.warning + '" stroke-width="3"><title>' + xml(labels.audioStopped!) + '</title></path>' : "";
const key = (body: string, size: number): string => svgDocument(size,
  rect(0, 0, 144, 144, 12, palette.background) + '<g text-anchor="middle" font-family="Segoe UI,Malgun Gothic,sans-serif" font-weight="600">' + body + '</g>');
const bounded = (value: number, max: number): number => Number.isFinite(value) ? Math.max(0, Math.min(max, Math.trunc(value))) : 0;

export function micSvg(lamp: Lamp, labels: Labels, stopped = false, unknownValue = false, size = 144): string {
  const on = lamp === "on" || lamp === "muted-on", muted = lamp.startsWith("muted-");
  let body: string, label = unknownValue ? "—" : "";
  if (["on", "off", "muted-on", "muted-off"].includes(lamp)) {
    const color = muted ? palette.danger : on ? palette.lampOn : palette.lampOff;
    body = '<circle cx="72" cy="39" r="30" fill="' + color + '"/>' + placed(glyph("mic"), 51, 16, 42);
    if (muted) body += stroke('<path d="M49 18l46 43"/>', palette.white, 3);
    label = muted ? labels[on ? "originalOn" : "originalOff"]! : on ? labels.on! : labels.off!;
    if (muted) body += rect(22, 72, 100, 17, 5, palette.danger) + text(labels.muted!, 85, 12, palette.background);
  } else if (lamp === "disconnected") {
    body = stroke('<path d="M32 16v19l24 24 14-14-24-24H32M112 73V54L88 30 74 44l24 24h14M45 21l8-8M88 68l-8 8"/>', palette.dimText, 4)
      + stroke('<path d="M65 17l4 9M97 16l-7 7M34 65l8-3"/>', palette.danger, 3);
  } else if (lamp === "disabled") {
    body = rect(48, 37, 48, 38, 8, palette.lampOff)
      + stroke('<path d="M57 37V25a15 15 0 0 1 30 0v12M72 52v10"/>', palette.dimText, 4);
  } else if (lamp === "missing" || lamp === "duplicate") {
    body = '<rect x="43" y="13" width="58" height="58" rx="15" fill="none" stroke="' + palette.warning + '" stroke-width="3" stroke-dasharray="6 5"/>'
      + text(lamp === "duplicate" ? "!" : "?", 56, 37, palette.warning);
  } else if (lamp === "version") {
    body = stroke('<path d="M72 12l32 58H40zM72 32v17M72 59v1"/>', palette.warning, 4);
  } else {
    body = stroke('<path d="M95 28a27 27 0 1 0 2 30M95 14v16H79"/>', palette.accent, 4);
  }
  return key(body + pause(stopped, labels) + text(label, unknownValue ? 100 : muted ? 103 : 92, unknownValue ? 28 : muted ? 13 : 18), size);
}

export function actionSvg(lamp: ActionLamp, labels: Labels, count = 0, total = 0, stopped = false, group: "mic" | "fx" = "mic", size = 144, index = 1): string {
  count = bounded(count, 8); total = bounded(total, 8);
  index = Math.max(1, bounded(index, 5));
  const muted = lamp === "group-muted", off = lamp.endsWith("off");
  const color = muted || lamp === "plugin-off" || lamp === "plugin-all-off" ? palette.danger : lamp === "mixed" || lamp === "plugin-all-mixed" ? palette.warning
    : lamp === "plugin-on" || lamp === "plugin-all-on" ? palette.accent : off ? palette.lampOff : palette.lampOn;
  let body: string, label = labels[off ? "off" : "on"]!, detail = "";
  if (lamp.startsWith("all") || lamp === "mixed") {
    body = [0, 1, 2].map(i => {
      const lit = lamp === "all-on" || (lamp === "mixed" && i < 2);
      return rect(21 + i * 36, 12, 30, 48, 12, lit ? palette.lampOn : palette.lampOff) + placed(glyph("mic"), 24 + i * 36, 22, 24);
    }).join("");
    if (lamp === "mixed") body += rect(55, 64, 34, 3, 1.5, palette.warning);
    label = labels[lamp === "mixed" ? "someOn" : off ? "allOff" : "allOn"]!; detail = count + "/" + total;
  } else if (lamp.startsWith("group")) {
    body = placed(glyph(group === "mic" ? "mic-mute-group" : "fx-mute-group", color), 43, 7, 58);
    if (muted) body += stroke('<path d="M45 10l54 53"/>', palette.danger, 4);
    label = labels[muted ? "muteState" : "unmuteState"]!; detail = labels.targets!.replace("{count}", String(count));
  } else if (lamp.startsWith("plugin-all")) {
    body = placed(glyph("plugin-group", color, index), 43, 7, 58);
    label = labels[lamp === "plugin-all-mixed" ? "someOn" : off ? "allOff" : "allOn"]!; detail = count + "/" + total;
  } else if (lamp.startsWith("plugin")) {
    body = placed(glyph("plugin-group", color, count), 43, 7, 58);
  } else {
    body = placed(glyph("status", off ? palette.dimText : palette.lampOn), 43, 7, 58);
    label = labels[off ? "audioStopped" : "connected"]!;
  }
  return key(body + pause(stopped, labels) + text(label, 82, 16) + text(detail, 102, 13, palette.dimText), size);
}

export function sendSvg(percent: number, badge: string, labels: Labels, muted = false, stopped = false, size = 144): string {
  percent = Math.round(Math.max(0, Math.min(100, Number.isFinite(percent) ? percent : 0)));
  const color = muted ? palette.danger : percent > 0 ? palette.lampOn : palette.lampOff;
  const body = placed(glyph("fx-send-step", color), 15, 9, 28)
    + (muted ? text(labels.muted!, 48, 10, palette.danger, 34) : "")
    + rect(80, 12, 49, 25, 6, palette.card2) + text(badge, 30, 16, palette.white, 104.5)
    + text(percent + "%", 79, 34)
    + rect(18, 91, 108, 7, 3.5, palette.lampOff) + rect(18, 91, percent * 1.08, 7, 3.5, color);
  return key(body + pause(stopped, labels), size);
}
export function encoderSvg(size = 144): string {
  return svgDocument(size, placed(glyph("fx-send"), 24, 24, 96));
}

// Geometric ON lettering makes the brand independent of fonts, in SVG and PNG.
export type BrandShape = { fill: string } & (
  { kind: "rect"; x: number; y: number; w: number; h: number; r: number }
  | { kind: "polygon"; points: readonly (readonly [number, number])[] }
);
export const brandShapes: readonly BrandShape[] = [
  { kind: "rect", x: 0, y: 0, w: 144, h: 144, r: 0, fill: palette.background },
  { kind: "rect", x: 12, y: 12, w: 120, h: 120, r: 26, fill: palette.brand },
  { kind: "rect", x: 29, y: 49, w: 38, h: 46, r: 12, fill: palette.white },
  { kind: "rect", x: 39, y: 59, w: 18, h: 26, r: 4, fill: palette.brand },
  { kind: "polygon", points: [[78,95],[78,49],[88,49],[105,77],[105,49],[115,49],[115,95],[105,95],[88,67],[88,95]], fill: palette.white }
];
export function brandSvg(size = 512): string {
  return svgDocument(size, brandShapes.map(s => s.kind === "rect" ? rect(s.x, s.y, s.w, s.h, s.r, s.fill)
    : '<polygon points="' + s.points.map(p => p.join(",")).join(" ") + '" fill="' + s.fill + '"/>').join(""));
}
export function categorySvg(size = 28): string {
  return svgDocument(size, '<g fill="none" stroke="#FFFFFF" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="7" width="7" height="10" rx="2.5"/><path d="M14 17V7l7 10V7"/></g>', 24);
}
