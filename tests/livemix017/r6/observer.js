// Test-only capture of the *rendered* stereo output, downstream of the real node.
class Observer extends AudioWorkletProcessor {
  constructor() {
    super(); this.buffer = null; this.position = 0;
    this.port.onmessage = e => {
      if (e.data.type !== 'measure') return;
      this.buffer = new Float32Array(Math.round(e.data.seconds * sampleRate) * 2);
      this.position = 0; this.start = currentTime; this.firstAudible = null;
      this.silentBlocks = 0; this.longestSilence = 0; this.silenceRun = 0;
    };
  }
  process(inputs, outputs) {
    const input = inputs[0];
    if (!input?.length) return true;
    let peak = 0;
    for (let i = 0; i < input[0].length; i++) {
      const l = input[0][i], r = input[1]?.[i] ?? 0;
      if (outputs[0]?.[0]) outputs[0][0][i] = l;
      if (outputs[0]?.[1]) outputs[0][1][i] = r;
      peak = Math.max(peak, Math.abs(l), Math.abs(r));
      if (this.buffer && this.position < this.buffer.length) {
        this.buffer[this.position++] = l; this.buffer[this.position++] = r;
        if (this.firstAudible === null && Math.max(Math.abs(l),Math.abs(r)) > 0.001) this.firstAudible = currentTime + i/sampleRate;
      }
    }
    if (this.buffer) {
      if (peak < 0.00001) { this.silentBlocks++; this.silenceRun += input[0].length; this.longestSilence = Math.max(this.longestSilence,this.silenceRun); }
      else this.silenceRun = 0;
      if (this.position === this.buffer.length) {
        const buffer = this.buffer.buffer;
        this.port.postMessage({buffer,rate:sampleRate,start:this.start,firstAudible:this.firstAudible,
          silentBlocks:this.silentBlocks,longestSilenceMs:1000*this.longestSilence/sampleRate},[buffer]);
        this.buffer = null;
      }
    }
    return true;
  }
}
registerProcessor('r6-observer',Observer);
