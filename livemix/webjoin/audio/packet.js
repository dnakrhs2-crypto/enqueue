export const HEADER_BYTES = 48;
export const MONO_BYTES = 288;
export const STEREO_BYTES = 768;
export const CAPABILITIES = Object.freeze(["packet_v2", "roster_ack", "star_3", "pcm16_mono", "web_guest"]);
export const Kind = Object.freeze({ audio: 1, ping: 4, pong: 5, bye: 6, punch: 8, offer: 9,
  offerAck: 10, roster: 11, rosterAck: 12, streamRestart: 13 });
const decoder = new TextDecoder("utf-8", { fatal: true });
const encoder = new TextEncoder();
const bad = () => { throw new Error("packet"); };
export const validRate = rate => rate === 44100 || rate === 48000;
export const u32 = n => Number.isInteger(n) && n > 0 && n <= 0xffffffff;
export function sessionValue(session) {
  if (typeof session === "bigint") return session;
  if (typeof session !== "string" || !/^[0-9a-f]{16}$/.test(session)) return bad();
  return BigInt("0x" + session);
}
function bytesOf(data) {
  return data instanceof ArrayBuffer ? new Uint8Array(data) : new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
}
export function readPacket(data, session = null) {
  const bytes = bytesOf(data);
  if (bytes.byteLength < 48 || bytes.byteLength > 1200) return bad();
  const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (v.getUint32(0, true) !== 0x42434d4c || v.getUint8(4) !== 2) return bad();
  const h = { kind: v.getUint8(5), flags: v.getUint16(6, true), session: v.getBigUint64(8, true),
    origin: v.getUint32(16, true), seq: v.getUint32(20, true), firstFrame: v.getBigUint64(24, true),
    frames: v.getUint16(32, true), payloadBytes: v.getUint16(34, true), linkSender: v.getUint32(36, true),
    streamEpoch: v.getUint32(40, true), rosterRevision: v.getUint32(44, true), view: v, bytes };
  if (48 + h.payloadBytes !== bytes.byteLength || !h.origin || !h.linkSender ||
      (session !== null && h.session !== sessionValue(session))) return bad();
  if (h.kind === Kind.audio) {
    if (h.frames !== 120 || h.firstFrame % 120n !== 0n || !h.streamEpoch || !h.rosterRevision ||
        !((h.flags === 1 && h.payloadBytes === 720) || (h.flags === 2 && h.payloadBytes === 240))) return bad();
  } else {
    if (h.flags || h.frames || h.firstFrame || h.origin !== h.linkSender ||
        (h.kind === Kind.streamRestart ? !h.streamEpoch : h.streamEpoch !== 0)) return bad();
    const sizes = { 4: 8, 5: 8, 6: 0, 8: 0, 9: 16, 10: 16, 12: 0, 13: 12 };
    if (h.kind === Kind.roster) {
      if (h.payloadBytes < 8 || h.payloadBytes > 674 || !h.rosterRevision) return bad();
    } else if (!(h.kind in sizes) || h.payloadBytes !== sizes[h.kind]) return bad();
  }
  return h;
}
export function writeHeader(view, h, kind, flags, payloadBytes) {
  view.setUint32(0, 0x42434d4c, true); view.setUint8(4, 2); view.setUint8(5, kind);
  view.setUint16(6, flags, true); view.setBigUint64(8, sessionValue(h.session), true);
  view.setUint32(16, h.origin, true); view.setUint32(20, h.seq >>> 0, true);
  view.setBigUint64(24, kind === Kind.audio ? h.firstFrame : 0n, true);
  view.setUint16(32, kind === Kind.audio ? 120 : 0, true); view.setUint16(34, payloadBytes, true);
  view.setUint32(36, h.linkSender ?? h.origin, true);
  view.setUint32(40, kind === Kind.audio || kind === Kind.streamRestart ? h.streamEpoch : 0, true);
  view.setUint32(44, h.rosterRevision, true);
}
function quantize(value, scale) {
  if (!Number.isFinite(value)) return 0;
  value = Math.max(-1, Math.min(1, value)) * scale;
  return Math.max(-scale, Math.min(scale - 1, Math.sign(value) * Math.floor(Math.abs(value) + 0.5)));
}
export function encodeAudio(h, samples, mono = true, buffer = new ArrayBuffer(mono ? MONO_BYTES : STEREO_BYTES)) {
  const view = new DataView(buffer);
  if (buffer.byteLength !== (mono ? MONO_BYTES : STEREO_BYTES) || samples.length < (mono ? 120 : 240)) return bad();
  writeHeader(view, h, Kind.audio, mono ? 2 : 1, mono ? 240 : 720);
  for (let i = 0; i < (mono ? 120 : 240); i++) {
    const sample = quantize(samples[i], mono ? 32768 : 8388608);
    if (mono) view.setInt16(48 + i * 2, sample, true);
    else {
      view.setUint8(48 + i * 3, sample & 255);
      view.setUint8(49 + i * 3, (sample >> 8) & 255);
      view.setUint8(50 + i * 3, (sample >> 16) & 255);
    }
  }
  return buffer;
}
export function decodeAudio(h, output) {
  if (h.kind !== Kind.audio || output.length < 240) return bad();
  for (let f = 0; f < 120; f++) {
    if (h.flags === 2) output[f * 2] = output[f * 2 + 1] = h.view.getInt16(48 + f * 2, true) / 32768;
    else for (let c = 0; c < 2; c++) {
      const p = 48 + (f * 2 + c) * 3;
      const value = h.view.getUint8(p) | (h.view.getUint8(p + 1) << 8) | (h.view.getInt8(p + 2) << 16);
      output[f * 2 + c] = value / 8388608;
    }
  }
  return output;
}
export function controlPacket(h, kind, payload = new Uint8Array(0)) {
  const buffer = new ArrayBuffer(48 + payload.byteLength);
  writeHeader(new DataView(buffer), h, kind, 0, payload.byteLength);
  new Uint8Array(buffer, 48).set(payload);
  return buffer;
}
export function rosterAck(roster, selfId) {
  return controlPacket({ ...roster, origin: selfId, linkSender: selfId }, Kind.rosterAck);
}
export function readRoster(h, { selfId, wireRate, allowedIds = null, previous = [] } = {}) {
  if (h.kind !== Kind.roster || h.origin !== 1 || h.linkSender !== 1) return bad();
  const v = h.view, rate = v.getUint32(48, true), count = v.getUint8(52);
  if (!validRate(rate) || (wireRate && rate !== wireRate) || count < 1 || count > 3 ||
      v.getUint8(53) !== 1 || v.getUint16(54, true)) return bad();
  const members = [];
  let offset = 56, lastId = 0, webCount = 0;
  for (let i = 0; i < count; i++) {
    if (offset + 30 > v.byteLength) return bad();
    const id = v.getUint32(offset, true), epoch = v.getUint32(offset + 4, true);
    const mask = v.getUint32(offset + 8, true), kind = v.getUint8(offset + 24), state = v.getUint8(offset + 25);
    const nickBytes = v.getUint16(offset + 26, true), versionBytes = v.getUint8(offset + 28);
    if (id <= lastId || !epoch || (mask & 31) !== 31 || (kind !== 1 && kind !== 2) ||
        (state !== 1 && state !== 2) || !nickBytes || nickBytes > 160 || versionBytes > 32 || v.getUint8(offset + 29) ||
        offset + 30 + nickBytes + versionBytes > v.byteLength || (allowedIds && !allowedIds.includes(id))) return bad();
    const start = h.bytes.byteOffset + offset + 30;
    const nick = decoder.decode(new Uint8Array(h.bytes.buffer, start, nickBytes));
    const appVersion = decoder.decode(new Uint8Array(h.bytes.buffer, start + nickBytes, versionBytes));
    if (nick !== nick.trim() || /[\u0000-\u001f\u007f-\u009f]/u.test(nick) || [...nick].length > 40 ||
        /[^\x20-\x7e]/.test(appVersion)) return bad();
    const old = previous.find(m => m.participantId === id);
    if (old && (epoch < old.streamEpoch || old.kind !== (kind === 1 ? "native" : "web"))) return bad();
    if (kind === 2) webCount++;
    if ((id === 1 && kind !== 1) || (selfId && (id === selfId) !== (kind === 2))) return bad();
    members.push({ participantId: id, streamEpoch: epoch, capabilities: mask,
      deviceRate: v.getUint32(offset + 12, true), inputLatencyUs: v.getUint32(offset + 16, true),
      outputLatencyUs: v.getUint32(offset + 20, true), kind: kind === 1 ? "native" : "web",
      state: state === 1 ? "connecting" : "connected", nick, appVersion });
    lastId = id; offset += 30 + nickBytes + versionBytes;
  }
  if (offset !== v.byteLength || webCount !== 1 || !members.some(m => m.participantId === 1) ||
      (selfId && !members.some(m => m.participantId === selfId))) return bad();
  return { wireRate: rate, rosterRevision: h.rosterRevision, members };
}
// Also used by the headless host fixtures; the browser guest only receives rosters.
export function encodeRoster(h, wireRate, members) {
  const encoded = members.map(m => ({ m, nick: encoder.encode(m.nick), version: encoder.encode(m.appVersion ?? "0.17.0") }));
  const payload = new Uint8Array(8 + encoded.reduce((sum, e) => sum + 30 + e.nick.length + e.version.length, 0));
  const v = new DataView(payload.buffer);
  v.setUint32(0, wireRate, true); v.setUint8(4, members.length); v.setUint8(5, 1);
  let offset = 8;
  for (const { m, nick, version } of encoded) {
    for (const [p, n] of [[0, m.participantId], [4, m.streamEpoch], [8, m.capabilities ?? 31],
      [12, m.deviceRate ?? wireRate], [16, m.inputLatencyUs ?? 0], [20, m.outputLatencyUs ?? 0]]) v.setUint32(offset + p, n, true);
    v.setUint8(offset + 24, m.kind === "web" ? 2 : 1); v.setUint8(offset + 25, m.state === "connecting" ? 1 : 2);
    v.setUint16(offset + 26, nick.length, true); v.setUint8(offset + 28, version.length);
    payload.set(nick, offset + 30); payload.set(version, offset + 30 + nick.length);
    offset += 30 + nick.length + version.length;
  }
  return controlPacket(h, Kind.roster, payload);
}
export function validDownlink(h, selfId, revision, members) {
  return h.kind === Kind.audio && h.flags === 1 && h.linkSender === 1 && h.origin !== selfId &&
    h.rosterRevision <= revision && members.some(m => m.participantId === h.origin &&
      m.kind === "native" && m.streamEpoch === h.streamEpoch);
}
