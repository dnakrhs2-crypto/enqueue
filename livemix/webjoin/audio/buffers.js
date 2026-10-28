// Bounded storage shared by the transport and render threads. No shared memory.
export const FRAMES = 120;
// How long the guest's own voice may wait on its way out. A phone's audio thread, worker and radio pause for tens to
// hundreds of milliseconds; with 20 ms here (until 10/28) the page threw that voice away itself, and everyone on a PC
// heard the phone guest cut out. The host's web playout absorbs the delay, up to 400 ms.
export const MAX_AGE_MS = 400;
export const audioPackets = rate => Math.ceil(rate * MAX_AGE_MS / 1000 / FRAMES);
export const audioBudget = rate => audioPackets(rate) * 288;
export const expired = (createdAtMs, now = Date.now()) => now - createdAtMs > MAX_AGE_MS;

// Each lease owns its envelope, transfer list and views. Reconstruct detached views
// only in the message handler, never in AudioWorkletProcessor.process().
export class TransferPool {
  constructor(count, bytes, type, generation) {
    this.slots = Array.from({ length: count }, () => {
      const buffer = new ArrayBuffer(bytes);
      return { busy: false, buffer, floats: new Float32Array(buffer), bytes: new Uint8Array(buffer),
        view: new DataView(buffer), transfer: [buffer], message: { type, generation, buffer } };
    });
    this.byteLength = bytes;
  }
  take() {
    for (let i = 0; i < this.slots.length; i++) {
      const slot = this.slots[i];
      if (!slot.busy) { slot.busy = true; return slot; }
    }
    return null;
  }
  borrowed() {
    let count = 0;
    for (let i = 0; i < this.slots.length; i++) if (this.slots[i].busy) count++;
    return count;
  }
  recycle(buffer) {
    if (!(buffer instanceof ArrayBuffer) || buffer.byteLength !== this.byteLength) return false;
    for (let i = 0; i < this.slots.length; i++) {
      const slot = this.slots[i];
      if (slot.busy) {
        slot.buffer = buffer;
        slot.floats = new Float32Array(buffer);
        slot.bytes = new Uint8Array(buffer);
        slot.view = new DataView(buffer);
        slot.transfer[0] = buffer;
        slot.message.buffer = buffer;
        slot.busy = false;
        return true;
      }
    }
    return false;
  }
}

export class FrameFIFO {
  constructor(capacity, channels = 1) {
    this.capacity = capacity;
    this.channels = channels;
    this.data = new Float32Array(capacity * channels);
    this.clear();
  }
  clear() { this.read = 0; this.size = 0; }
  push(input, frames, offset = 0) {
    if (frames > this.capacity - this.size) return false;
    for (let f = 0; f < frames; f++) {
      const dest = ((this.read + this.size + f) % this.capacity) * this.channels;
      for (let c = 0; c < this.channels; c++) this.data[dest + c] = input[(offset + f) * this.channels + c];
    }
    this.size += frames;
    return true;
  }
  pop(output, frames, offset = 0) {
    if (frames > this.size) return false;
    for (let f = 0; f < frames; f++) {
      const source = ((this.read + f) % this.capacity) * this.channels;
      for (let c = 0; c < this.channels; c++) output[(offset + f) * this.channels + c] = this.data[source + c];
    }
    this.read = (this.read + frames) % this.capacity;
    this.size -= frames;
    return true;
  }
}

export function channelInfo(channel) {
  return { label: channel.label, protocol: channel.protocol, ordered: channel.ordered,
    maxRetransmits: channel.maxRetransmits, maxPacketLifeTime: channel.maxPacketLifeTime,
    negotiated: channel.negotiated };
}
export function validChannel(c) {
  return c.label === "livemix-audio-v2" && c.protocol === "livemix-pcm-v2" && c.ordered === false &&
    c.maxRetransmits === 0 && c.maxPacketLifeTime === null && c.negotiated === false;
}
export function normalChannelClose(event) {
  const error = event?.error ?? event;
  return error?.name === "OperationError" && /User-Initiated Abort/.test(error.message ?? "");
}
