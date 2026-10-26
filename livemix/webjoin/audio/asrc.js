// SPDX-License-Identifier: MIT
// Copyright (c) 2026 LiveMix contributors
// Direct JS port of livemix/shared/lm_asrc.{h,c}; full MIT notice in lm_asrc.h.
export const TAPS = 32;
export const PHASES = 512;
export const DELAY = 16;
const clamp = (x, lo, hi) => Math.max(lo, Math.min(hi, x));
function bessel(x) {
  let sum = 1, term = 1;
  for (let k = 1; k < 64; k++) {
    term *= x * x * 0.25 / (k * k);
    sum += term;
    if (term < sum * 1e-16) break;
  }
  return sum;
}

export class ASRC {
  constructor(channels, inputRate, outputRate) {
    if (!Number.isInteger(channels) || channels < 1 || channels > 2 ||
        !Number.isFinite(inputRate) || !Number.isFinite(outputRate) || inputRate <= 0 || outputRate <= 0)
      throw new RangeError("rate");
    this.channels = channels;
    this.nominalRatio = this.ratio = inputRate / outputRate;
    this.kernel = new Float32Array((PHASES + 1) * TAPS);
    this.history = new Float32Array(TAPS * channels);
    const cutoff = 0.45 * Math.min(1, outputRate / inputRate), scale = 1 / bessel(8.6);
    const weights = new Float64Array(TAPS);
    for (let phase = 0; phase <= PHASES; phase++) {
      let total = 0;
      for (let tap = 0; tap < TAPS; tap++) {
        const d = tap + phase / PHASES - DELAY, position = d / (TAPS * 0.5);
        const window = bessel(8.6 * Math.sqrt(Math.max(0, 1 - position * position))) * scale;
        const sinc = Math.abs(d) < 1e-12 ? 2 * cutoff : Math.sin(2 * Math.PI * cutoff * d) / (Math.PI * d);
        weights[tap] = sinc * window;
        total += weights[tap];
      }
      for (let tap = 0; tap < TAPS; tap++) this.kernel[phase * TAPS + tap] = weights[tap] / total;
    }
    this.reset();
  }
  reset() { this.newest = TAPS - 1; this.phase = 0; this.pending = 1; this.used = 0; this.history.fill(0); }
  setCorrection(ppm) {
    const ratio = this.nominalRatio * (1 + ppm * 1e-6);
    if (Number.isFinite(ratio) && ratio > 0) this.ratio = ratio;
  }
  // Offsets/counts are frames. 'used' is retained on the instance (no result object).
  process(input, inFrames, output, outFrames, inOffset = 0, outOffset = 0) {
    let used = 0, made = 0;
    while (made < outFrames) {
      while (this.pending >= 1) {
        if (used >= inFrames) { this.used = used; return made; }
        this.newest = (this.newest + 1) & (TAPS - 1);
        for (let c = 0; c < this.channels; c++)
          this.history[this.newest * this.channels + c] = input[(inOffset + used) * this.channels + c];
        used++;
        this.pending--;
      }
      const position = this.phase * PHASES, phase = Math.floor(position), blend = position - phase;
      for (let c = 0; c < this.channels; c++) {
        let sample = 0;
        for (let tap = 0; tap < TAPS; tap++) {
          const a = this.kernel[phase * TAPS + tap];
          const weight = a + blend * (this.kernel[(phase + 1) * TAPS + tap] - a);
          sample += this.history[((this.newest - tap) & (TAPS - 1)) * this.channels + c] * weight;
        }
        output[(outOffset + made) * this.channels + c] = sample;
      }
      made++;
      this.phase += this.ratio;
      this.pending = Math.floor(this.phase);
      this.phase -= this.pending;
    }
    this.used = used;
    return made;
  }
}

export class Drift {
  constructor(target) { this.reset(target); }
  reset(target) {
    this.target = target > 0 && Number.isFinite(target) ? target : 1;
    this.kp = 4320 / this.target; this.ki = 144 / this.target;
    this.integ = this.lp = this.ppm = this.elapsed = 0;
    this.maxPpm = 1000; this.slew = 20; this.acquireSlew = 200; this.acquireSeconds = 10;
    this.primed = false;
  }
  update(fill, dt) {
    if (!Number.isFinite(fill) || !Number.isFinite(dt) || dt <= 0) return this.ppm;
    const error = fill - this.target;
    this.lp = this.primed ? this.lp - Math.expm1(-dt) * (error - this.lp) : error;
    this.primed = true;
    const slew = this.elapsed < this.acquireSeconds - 1e-9 ? this.acquireSlew : this.slew;
    this.elapsed += dt;
    const wanted = this.kp * this.lp + this.integ;
    const limited = clamp(wanted, -this.maxPpm, this.maxPpm);
    const applied = clamp(limited, this.ppm - slew * dt, this.ppm + slew * dt);
    const step = this.ki * this.lp * dt;
    if ((wanted - applied) * step <= 0) this.integ = clamp(this.integ + step, -this.maxPpm, this.maxPpm);
    this.ppm = applied;
    return applied;
  }
}
