import { ASRC, Drift } from "./asrc.js";
import { FRAMES } from "./buffers.js";

// The freshest arrival of the last two seconds is the zero of the delay spread; a sender
// clock up to +/-1000 ppm away moves it by at most 2 ms over that window.
const BASE_BUCKETS = 8, BASE_BUCKET_MS = 250;
// A phone network stalls now and then (10/27 Fold7 on LTE and Wi-Fi: the host's audio cut out and
// resumed every so often). Once a stall is seen the buffer covers it for five minutes, as an
// underrun's raised target is held in CollabPlayout, so the next stall of that size plays through.
const PEAK_SECONDS = 300, STEP_DOWN_SECONDS = 2;

// One instance per origin/epoch. Packet slots and DSP scratch are allocated on
// configuration, never on the rendering path. Arrival timestamps size the buffer;
// the feedback clock is the device's rendered-frame clock, independently per peer.
export class Playout {
  constructor(wireRate, deviceRate) {
    this.wireRate = wireRate; this.deviceRate = deviceRate;
    this.floor = wireRate * 0.020; this.ceiling = wireRate * 0.16;
    // The ceiling plus a whole 2048-frame host block of burst still fits the ring.
    this.capacity = Math.ceil(wireRate * 0.4 / FRAMES);
    this.slots = Array.from({ length: this.capacity }, () => ({ frame: 0, ready: false, pcm: new Float32Array(240) }));
    this.packet = new Float32Array(240); this.output = new Float32Array(240);
    this.baseBucket = new Float64Array(BASE_BUCKETS); this.baseDelay = new Float64Array(BASE_BUCKETS);
    this.peakSecond = new Float64Array(PEAK_SECONDS); this.peakSpread = new Float64Array(PEAK_SECONDS);
    this.asrc = new ASRC(2, wireRate, deviceRate);
    this.drift = new Drift(this.floor);
    this.releaseFrames = Math.max(1, Math.round(deviceRate * 0.005));
    this.reset(0);
  }
  reset(epoch) {
    this.epoch = epoch;
    for (let i = 0; i < this.capacity; i++) this.slots[i].ready = false;
    this.anchor = null; this.cursor = null; this.newest = -Infinity;
    this.readyFrames = 0; this.position = FRAMES; this.packetReal = false;
    this.target = this.floor; this.playing = false;
    this.hold = 0; this.holdSeconds = 0; this.stableSeconds = 0;
    this.baseBucket.fill(-1); this.base = null; this.peakSecond.fill(-1); this.second = -1; this.spreadMs = 0;
    this.arrivals = 0; this.lastArrival = NaN;
    this.fadeIn = FRAMES; this.release = 0;
    this.lastL = this.lastR = this.inputL = this.inputR = 0;
    this.releaseL = this.releaseR = 0;
    this.late = this.dropped = this.received = this.lost = this.underruns = 0;
    this.arrivalNs = 0n;
    this.asrc.reset(); this.asrc.setCorrection(0);
    this.drift.reset(this.target);
    // CollabPlayout's operating gains allow recovery around +/-1000 ppm clocks.
    this.drift.kp = 6; this.drift.ki = 0.3;
    this.drift.maxPpm = this.drift.slew = this.drift.acquireSlew = 2000;
  }
  index(frame) { return ((frame / FRAMES) % this.capacity + this.capacity) % this.capacity; }
  queued() { return this.readyFrames + (this.packetReal ? FRAMES - this.position : 0); }
  // How much later than the freshest recent packet this one arrived, as CollabPlayout measures
  // its floor, but over the whole call. A host's device block arrives as one burst, so the
  // spread includes the block as well as the network's own stalls.
  measure(frame, arrivalNs) {
    const now = Number(arrivalNs) / 1e6, delay = now - frame * 1000 / this.wireRate;
    // A device block's packets arrive together; the first start waits to see the bursts repeat.
    if (!(Math.abs(now - this.lastArrival) <= 1)) this.arrivals++;
    this.lastArrival = now;
    if (!Number.isFinite(delay)) return;
    if (this.base !== null) {
      const spread = Math.max(0, delay - this.base), second = Math.floor(now / 1000), i = second % PEAK_SECONDS;
      if (this.peakSecond[i] !== second) { this.peakSecond[i] = second; this.peakSpread[i] = spread; }
      else if (spread > this.peakSpread[i]) this.peakSpread[i] = spread;
      if (second !== this.second) {
        this.second = second; this.spreadMs = 0;
        for (let j = 0; j < PEAK_SECONDS; j++)
          if (this.peakSecond[j] >= 0 && second - this.peakSecond[j] < PEAK_SECONDS) this.spreadMs = Math.max(this.spreadMs, this.peakSpread[j]);
      } else if (spread > this.spreadMs) this.spreadMs = spread;
    }
    // The base is updated after the spread: a stall longer than the window still counts in full.
    const bucket = Math.floor(now / BASE_BUCKET_MS), b = bucket % BASE_BUCKETS;
    if (this.baseBucket[b] !== bucket) { this.baseBucket[b] = bucket; this.baseDelay[b] = delay; }
    else if (delay < this.baseDelay[b]) this.baseDelay[b] = delay;
    let base = Infinity;
    for (let j = 0; j < BASE_BUCKETS; j++)
      if (this.baseBucket[j] >= 0 && bucket - this.baseBucket[j] < BASE_BUCKETS) base = Math.min(base, this.baseDelay[j]);
    this.base = base;
  }
  needed() {
    const measured = this.spreadMs * this.wireRate / 1000 + FRAMES;
    return Math.min(this.ceiling, Math.max(this.floor, this.holdSeconds > 0 ? this.hold : 0, measured));
  }
  push(message, pcm) {
    if (message.streamEpoch !== this.epoch || message.frames !== FRAMES || pcm.length !== 240) return false;
    if (this.anchor === null) this.anchor = message.firstFrame;
    const frame = Number(message.firstFrame - this.anchor);
    if (!Number.isSafeInteger(frame) || frame % FRAMES) { this.dropped++; return false; }
    // Late packets belong to the delay distribution too.
    this.measure(frame, message.arrivalNs);
    if (this.cursor !== null && frame < this.cursor) { this.late++; return false; }
    if (frame < this.newest - (this.capacity - 1) * FRAMES) { this.late++; return false; }
    const boundary = this.cursor ?? this.newest;
    if (Number.isFinite(boundary) && frame - boundary >= this.capacity * FRAMES) {
      this.rebuffer(true);
      this.cursor = frame;
      this.dropped++;
    }
    const slot = this.slots[this.index(frame)];
    if (slot.ready && slot.frame === frame) { this.dropped++; return false; }
    if (slot.ready) { this.readyFrames -= FRAMES; this.dropped++; }
    slot.pcm.set(pcm); slot.frame = frame; slot.ready = true;
    this.readyFrames += FRAMES; this.newest = Math.max(this.newest, frame);
    this.arrivalNs = message.arrivalNs;
    this.received++;
    return true;
  }
  rebuffer(clear = false) {
    this.releaseL = this.lastL; this.releaseR = this.lastR;
    this.release = this.releaseFrames;
    this.playing = false; this.position = FRAMES; this.packetReal = false;
    this.fadeIn = FRAMES; this.asrc.reset(); this.stableSeconds = 0;
    this.drift.lp = (this.drift.ppm - this.drift.integ) / this.drift.kp;
    this.drift.primed = true;
    if (clear) {
      for (let i = 0; i < this.capacity; i++) this.slots[i].ready = false;
      this.readyFrames = 0;
    }
  }
  start(frames) {
    const wanted = Math.ceil(this.target + frames * this.asrc.ratio + FRAMES * 0.5);
    // Two repeats of the burst have measured a 2048-frame host block before the first start.
    if (this.release || this.arrivals < 3 || this.queued() < wanted) return;
    let earliest = Infinity;
    for (let i = 0; i < this.capacity; i++) if (this.slots[i].ready) earliest = Math.min(earliest, this.slots[i].frame);
    if (!Number.isFinite(earliest)) return;
    // Seek a backlog at packet boundaries; do not make the PI loop consume it.
    for (let i = 0; i < this.capacity && this.queued() >= wanted + FRAMES; i++, earliest += FRAMES) {
      const slot = this.slots[this.index(earliest)];
      if (slot.ready && slot.frame === earliest) { slot.ready = false; this.readyFrames -= FRAMES; this.dropped++; }
    }
    this.skip = Math.min(FRAMES - 1, Math.max(0, this.queued() - wanted));
    this.cursor = earliest; this.playing = true; this.fadeIn = FRAMES;
  }
  load() {
    const slot = this.slots[this.index(this.cursor)];
    const found = slot.ready && slot.frame === this.cursor;
    if (!found && !this.readyFrames) {
      this.underruns++; this.lost++;
      this.target = Math.min(this.ceiling, this.target + FRAMES);
      this.hold = this.target; this.holdSeconds = PEAK_SECONDS;
      this.rebuffer();
      return false;
    }
    if (found) {
      this.packet.set(slot.pcm); slot.ready = false; this.readyFrames -= FRAMES;
      for (let f = this.skip; f < FRAMES && this.fadeIn > 0; f++) {
        const gain = 1 - --this.fadeIn / FRAMES;
        this.packet[f * 2] *= gain; this.packet[f * 2 + 1] *= gain;
      }
    } else {
      this.lost++; this.concealed = true;
      for (let f = 0; f < FRAMES; f++) {
        const gain = 1 - (f + 1) / FRAMES;
        this.packet[f * 2] = this.inputL * gain; this.packet[f * 2 + 1] = this.inputR * gain;
      }
      this.fadeIn = FRAMES;
    }
    this.inputL = this.packet[238]; this.inputR = this.packet[239];
    this.position = this.skip; this.skip = 0; this.packetReal = found;
    this.cursor += FRAMES;
    return true;
  }
  render(left, right, frames, offset = 0) {
    for (let base = 0; base < frames; base += FRAMES) {
      const n = Math.min(FRAMES, frames - base);
      this.output.fill(0); this.concealed = false;
      // A larger measured spread applies at once: a refill waits for it, playback slews to it.
      const needed = this.needed();
      if (needed > this.target) this.target = needed;
      if (!this.playing) this.start(n);
      let made = 0;
      while (this.playing && made < n) {
        if (this.position === FRAMES && !this.load()) break;
        made += this.asrc.process(this.packet, FRAMES - this.position, this.output, n - made, this.position, made);
        this.position += this.asrc.used;
      }
      if (!this.playing && this.release > 0) {
        if (made > 0) { this.releaseL = this.output[made * 2 - 2]; this.releaseR = this.output[made * 2 - 1]; }
        for (let f = made; f < n && this.release > 0; f++) {
          const gain = --this.release / this.releaseFrames;
          this.output[f * 2] = this.releaseL * gain; this.output[f * 2 + 1] = this.releaseR * gain;
        }
      }
      const dt = n / this.deviceRate;
      this.holdSeconds = Math.max(0, this.holdSeconds - dt);
      if (this.playing && !this.concealed && !this.fadeIn) {
        // After a calm stretch the buffer steps back down toward what the last five minutes needed.
        this.stableSeconds += dt;
        if (this.stableSeconds >= STEP_DOWN_SECONDS) {
          this.stableSeconds = 0;
          this.target = Math.max(needed, this.target - FRAMES);
        }
        this.drift.target = this.target;
        const error = this.queued() - this.target;
        const clockError = Math.sign(error) * Math.max(0, Math.abs(error) - FRAMES * 0.5);
        this.asrc.setCorrection(this.drift.update(this.target + clockError, dt));
      } else this.stableSeconds = 0;
      for (let f = 0; f < n; f++) {
        left[offset + base + f] = this.output[f * 2]; right[offset + base + f] = this.output[f * 2 + 1];
      }
      this.lastL = this.output[(n - 1) * 2]; this.lastR = this.output[(n - 1) * 2 + 1];
    }
  }
  stats(target) {
    target.underruns = this.underruns; target.late = this.late; target.dropped = this.dropped;
    target.lost = this.lost; target.received = this.received;
    target.bufferMs = this.queued() * 1000 / this.wireRate;
    target.targetMs = this.target * 1000 / this.wireRate;
    target.spreadMs = this.spreadMs;
    target.driftPpm = this.drift.ppm;
    return target;
  }
}
