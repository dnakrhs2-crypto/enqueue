import { ASRC } from "./asrc.js";
import { Playout } from "./playout.js";
import { FRAMES, FrameFIFO, TransferPool, audioPackets } from "./buffers.js";

// Exported core also runs in the deterministic node tests with real render sizes.
export class GuestAudio {
  constructor(generation, wireRate, deviceRate) {
    this.generation = generation; this.wireRate = wireRate; this.deviceRate = deviceRate;
    this.port = null; this.closed = false; this.enabled = true; this.active = false;
    this.epoch = 0; this.frame = 0; this.captureDropped = 0;
    this.captureASRC = new ASRC(1, deviceRate, wireRate);
    this.fifo = new FrameFIFO(FRAMES * 2);
    this.scratch = new Float32Array(FRAMES); this.silence = new Float32Array(FRAMES);
    this.capturePool = new TransferPool(audioPackets(wireRate), FRAMES * 4, "capture", generation);
    this.left = new Float32Array(FRAMES); this.right = new Float32Array(FRAMES);
    this.peers = Array.from({ length: 2 }, () => ({ id: 0, epoch: 0, gain: 1, target: 1,
      remaining: 0, retiring: false, tailL: 0, tailR: 0, tailRemaining: 0, playout: new Playout(wireRate, deviceRate) }));
    this.gains = Array.from({ length: 3 }, () => ({ id: 0, gain: 1 }));
    this.ramp = Math.max(1, Math.round(deviceRate * 0.020));
    this.peak = 0; this.statsFrames = 0;
    this.statsMessage = { type: "stats", generation, peak: 0, captureDropped: 0,
      people: [{ participantId: 0 }, { participantId: 0 }] };
  }
  message(m) {
    if (m.generation !== this.generation || this.closed) return;
    if (m.type === "audioPort") { this.port = m.port; this.port.onmessage = e => this.message(e.data); this.port.start?.(); }
    else if (m.type === "configure") {
      if (m.wireRate !== this.wireRate || m.deviceRate !== this.deviceRate) return;
      if (this.epoch !== m.streamEpoch) {
        this.epoch = m.streamEpoch; this.frame = 0; this.fifo.clear(); this.captureASRC.reset();
      }
      this.active = true;
    } else if (m.type === "mic") {
      this.enabled = !!m.enabled;
      if (!this.enabled) this.fifo.data.fill(0);
    }
    else if (m.type === "peerGain") this.setGain(m.participantId, m.gain);
    else if (m.type === "people") this.people(m.members);
    else if (m.type === "playback") {
      const pcm = new Float32Array(m.buffer);
      for (let i = 0; i < 2; i++) {
        const peer = this.peers[i];
        if (peer.id === m.origin && !peer.retiring) peer.playout.push(m, pcm);
      }
      this.port.postMessage({ type: "recycle", generation: this.generation, pool: "playback", buffer: m.buffer }, [m.buffer]);
    } else if (m.type === "recycle" && m.pool === "capture") this.capturePool.recycle(m.buffer);
    else if (m.type === "close") { this.closed = true; this.port?.close(); }
  }
  setGain(id, gain) {
    if (!Number.isInteger(id) || id <= 0 || id > 0xffffffff || !Number.isFinite(gain) || gain < 0 || gain > 1) return;
    let entry = this.gains.find(g => g.id === id) ?? this.gains.find(g => !g.id);
    if (!entry) entry = this.gains.find(g => !this.peers.some(p => p.id === g.id && !p.retiring));
    if (!entry) return;
    entry.id = id; entry.gain = gain;
    for (let i = 0; i < 2; i++) if (this.peers[i].id === id && !this.peers[i].retiring) {
      this.peers[i].target = gain; this.peers[i].remaining = this.ramp;
    }
  }
  people(members) {
    if (members.length > 2) return;
    for (let i = 0; i < 2; i++) {
      const peer = this.peers[i];
      if (peer.id && !members.some(m => m.participantId === peer.id)) {
        peer.retiring = true; peer.target = 0; peer.remaining = this.ramp;
      }
    }
    for (const member of members) {
      let peer = this.peers.find(p => p.id === member.participantId);
      if (!peer) peer = this.peers.find(p => !p.id || p.retiring);
      if (!peer) continue;
      if (peer.id !== member.participantId || peer.epoch !== member.streamEpoch) {
        // A full snapshot can replace a departing peer in the same two slots.
        // Retain its release tail without allocating a third playback/ASRC.
        if (peer.id) {
          peer.tailL = peer.playout.lastL * peer.gain; peer.tailR = peer.playout.lastR * peer.gain;
          peer.tailRemaining = this.ramp;
        }
        peer.playout.reset(member.streamEpoch);
        peer.id = member.participantId; peer.epoch = member.streamEpoch;
        peer.gain = this.gains.find(g => g.id === peer.id)?.gain ?? 1;
        peer.target = peer.gain; peer.remaining = 0;
      }
      peer.retiring = false;
    }
  }
  capture(input, frames) {
    let offset = 0;
    while (offset < frames) {
      const available = input ? frames - offset : Math.min(FRAMES, frames - offset);
      const made = this.captureASRC.process(input ?? this.silence, available, this.scratch, FRAMES, input ? offset : 0);
      offset += this.captureASRC.used;
      for (let f = 0; f < made; f++) {
        if (!this.enabled || !Number.isFinite(this.scratch[f])) this.scratch[f] = 0;
        this.peak = Math.max(this.peak, Math.abs(this.scratch[f]));
      }
      this.fifo.push(this.scratch, made);
      if (this.fifo.size >= FRAMES) {
        const slot = this.capturePool.take();
        this.fifo.pop(slot ? slot.floats : this.scratch, FRAMES);
        if (slot) {
          const m = slot.message;
          m.streamEpoch = this.epoch; m.firstFrame = BigInt(this.frame); m.frames = FRAMES; m.createdAtMs = Date.now();
          this.port.postMessage(m, slot.transfer);
        } else this.captureDropped++;
        this.frame += FRAMES;
      }
      if (!made && !this.captureASRC.used) break;
    }
  }
  process(input, left, right) {
    left.fill(0); right.fill(0);
    if (this.closed) return false;
    const frames = left.length;
    if (this.active && this.port) this.capture(input, frames);
    for (let i = 0; i < 2; i++) {
      const peer = this.peers[i];
      if (!peer.id) continue;
      for (let base = 0; base < frames; base += FRAMES) {
        const n = Math.min(FRAMES, frames - base);
        peer.playout.render(this.left, this.right, n);
        for (let f = 0; f < n; f++) {
          if (peer.remaining > 0) { peer.gain += (peer.target - peer.gain) / peer.remaining; peer.remaining--; }
          left[base + f] += this.left[f] * peer.gain; right[base + f] += this.right[f] * peer.gain;
          if (peer.tailRemaining > 0) {
            const release = --peer.tailRemaining / this.ramp;
            left[base + f] += peer.tailL * release; right[base + f] += peer.tailR * release;
          }
        }
      }
      if (peer.retiring && !peer.remaining) { peer.id = 0; peer.playout.reset(0); }
    }
    this.statsFrames += frames;
    if (this.port && this.statsFrames >= this.deviceRate / 10) {
      this.statsFrames = 0;
      this.statsMessage.peak = this.peak; this.statsMessage.captureDropped = this.captureDropped; this.peak = 0;
      for (let i = 0; i < 2; i++) {
        const result = this.statsMessage.people[i]; result.participantId = this.peers[i].id;
        this.peers[i].playout.stats(result);
      }
      this.port.postMessage(this.statsMessage);
    }
    return true;
  }
}

if (typeof registerProcessor === "function") {
  class LiveMixGuest extends AudioWorkletProcessor {
    constructor(options) {
      super();
      const config = options.processorOptions;
      this.audio = new GuestAudio(config.generation, config.wireRate, sampleRate);
      this.port.onmessage = e => this.audio.message(e.data);
    }
    process(inputs, outputs) {
      const out = outputs[0];
      if (!out || out.length < 2) return !this.audio.closed;
      return this.audio.process(inputs[0]?.[0] ?? null, out[0], out[1]);
    }
  }
  registerProcessor("livemix-guest", LiveMixGuest);
}
