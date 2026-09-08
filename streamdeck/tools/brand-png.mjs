// Dependency-free PNG output for the geometric brand mark only.
// SVG and PNG use identical primitives; 4× sampling smooths curved/diagonal edges.
import { deflateSync } from "node:zlib";
import { brandShapes } from "../src/ui/artwork.ts";
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
function contains(s, x, y) {
  if (s.kind === "rect") {
    if (x < s.x || y < s.y || x > s.x + s.w || y > s.y + s.h) return false;
    const dx = x - Math.max(s.x + s.r, Math.min(s.x + s.w - s.r, x));
    const dy = y - Math.max(s.y + s.r, Math.min(s.y + s.h - s.r, y));
    return dx * dx + dy * dy <= s.r * s.r;
  }
  let inside = false;
  for (let i = 0, j = s.points.length - 1; i < s.points.length; j = i++) {
    const [xi, yi] = s.points[i], [xj, yj] = s.points[j];
    if ((yi > y) !== (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) inside = !inside;
  }
  return inside;
}
export function brandPng(size) {
  const shapes = [...brandShapes].reverse().map(s => ({ ...s, rgb: s.fill.slice(1).match(/../g).map(v => parseInt(v, 16)) }));
  const header = Buffer.alloc(13); header.writeUInt32BE(size, 0); header.writeUInt32BE(size, 4); header[8] = 8; header[9] = 6;
  const pixels = Buffer.alloc((size * 4 + 1) * size), scale = 144 / size;
  for (let y = 0; y < size; y++) for (let x = 0; x < size; x++) {
    const rgb = [0, 0, 0];
    for (let sy = 0; sy < 4; sy++) for (let sx = 0; sx < 4; sx++) {
      const px = (x + (sx + .5) / 4) * scale, py = (y + (sy + .5) / 4) * scale;
      const s = shapes.find(s => contains(s, px, py));
      for (let c = 0; c < 3; c++) rgb[c] += s.rgb[c];
    }
    const offset = y * (size * 4 + 1) + 1 + x * 4;
    pixels.set([...rgb.map(v => Math.round(v / 16)), 255], offset);
  }
  return Buffer.concat([Buffer.from([137,80,78,71,13,10,26,10]), chunk("IHDR", header), chunk("IDAT", deflateSync(pixels)), chunk("IEND", Buffer.alloc(0))]);
}
