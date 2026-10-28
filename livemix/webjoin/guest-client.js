import { API_BASE, STUN } from "./signaling.js";
import { TransferPool, audioPackets, audioBudget, expired, validChannel, channelInfo, normalChannelClose } from "./audio/buffers.js";

export function parseInvite(hash) {
  if (typeof hash !== "string") throw new Error("invite");
  const values = new URLSearchParams(hash.startsWith("#") ? hash.slice(1) : hash);
  for (const key of ["v", "code", "invite", "generation"]) if (values.getAll(key).length !== 1) throw new Error("invite");
  const generation = values.get("generation");
  if (values.get("v") !== "2" || !/^[0-9]{4}$/.test(values.get("code")) ||
      !/^[0-9a-f]{64}$/.test(values.get("invite")) || !/^[1-9][0-9]{0,9}$/.test(generation) || Number(generation) > 0xffffffff)
    throw new Error("invite");
  return { code: values.get("code"), inviteToken: values.get("invite"), inviteGeneration: Number(generation) };
}
function microphoneCode(error) {
  return ({ NotAllowedError: "microphone_denied", NotFoundError: "microphone_missing",
    NotReadableError: "microphone_busy", OverconstrainedError: "microphone_constraints" })[error?.name] ?? "microphone";
}
// Android Chrome and Samsung Internet list their call routes as inputs ('Speakerphone', 'Headset earpiece', 'Bluetooth
// headset'). With Bluetooth earbuds connected the default input is the earbuds' call link: on gom's Galaxy Z Fold7 (both
// browsers, 10/27) it opened 3.9 s late, the page played nothing until Bluetooth was toggled, and a call link is mono.
// The phone's own mic ('Speakerphone') opened at once and the earbuds kept stereo. Unless the person picked a
// microphone, a listed Bluetooth headset therefore means the phone's mic.
export function phoneMicForBluetooth(devices) {
  const inputs = (devices ?? []).filter(d => d.kind === "audioinput");
  if (!inputs.some(d => /bluetooth/i.test(d.label))) return null;
  return inputs.find(d => /^speakerphone$/i.test(d.label.trim()))?.deviceId || null;
}
function openMicrophone(deviceId) {
  return navigator.mediaDevices.getUserMedia({ audio: { channelCount: 1, echoCancellation: false, noiseSuppression: false,
    autoGainControl: false, ...(deviceId ? { deviceId: { exact: deviceId } } : {}) }, video: false });
}
function connectionId() {
  return Array.from(crypto.getRandomValues(new Uint8Array(16)), v => v.toString(16).padStart(2, "0")).join("");
}

// 'testing' is an explicit dependency injection seam. Nothing reads URL query
// parameters, storage or globals to change the API, ICE servers or audio path.
export function createGuestClient({ onState = () => {}, onRoster = () => {}, onMeter = () => {}, onError = () => {}, testing = null } = {}) {
  return new GuestClient({ onState, onRoster, onMeter, onError }, testing);
}
class GuestClient {
  constructor(callbacks, testing) {
    this.callbacks = callbacks; this.testing = testing;
    this.state = "idle"; this.generation = 0; this.lifecycle = 0;
    this.disposed = false; this.ending = false; this.micEnabled = true;
    this.worker = null; this.context = null; this.stream = null; this.node = null; this.source = null; this.pc = null;
    this.relay = null; this.channel = null; this.relayTimer = null;
    this.rpcId = 0; this.pending = new Map(); this.gains = [];
    this.mainQueue = Promise.resolve(); this.joinTask = null; this.prepareTask = null;
    this.setState("idle");
  }
  setState(state) {
    this.state = state; this.callbacks.onState({ state, generation: this.generation });
  }
  diagnostics(value) {
    // Latest diagnostics only; no tokens and no unbounded audio/event history.
    if (value.type === "microphone") this.microphoneInfo = value;
    else if (value.type === "transport") this.transportInfo = value;
    else if (value.type === "metrics") this.audioMetrics = value;
    this.testing?.onDiagnostics?.(value);
  }
  ensureWorker() {
    if (this.disposed) throw new Error("disposed");
    if (this.worker) return;
    this.generation = 0;
    const worker = new Worker(new URL("./audio/worker.js", import.meta.url), { type: "module" });
    this.worker = worker;
    worker.onmessage = e => {
      if (this.worker !== worker) return;
      const m = e.data;
      if (m.type === "reply") {
        const request = this.pending.get(m.id);
        if (request) { this.pending.delete(m.id); m.code ? request.reject(new Error(m.code)) : request.resolve(m.result); }
        return;
      }
      if (this.ending || (m.type !== "accepted" && m.generation !== this.generation)) return;
      if (m.type === "meter") this.callbacks.onMeter({ peak: m.peak });
      else if (m.type === "metrics" || m.type === "transport") this.diagnostics(m);
      else {
        this.mainQueue = this.mainQueue.then(async () => {
          if (this.worker !== worker || this.ending) return;
          if (m.type === "accepted") await this.accept(m);
          else if (m.generation !== this.generation) return;
          else if (m.type === "remoteDescription") await this.remoteDescription(m);
          else if (m.type === "remoteCandidate") await this.remoteCandidate(m.signal);
          else if (m.type === "roster") this.callbacks.onRoster({ members: m.members, selfId: m.selfId });
          else if (m.type === "error") void this.fail(m.code);
          else if (m.type === "state") {
            if (m.state === "ended" || m.state === "rejected") void this.finish(m.state, false);
            else this.setState(m.state);
          }
        }).catch(() => { if (this.worker === worker && !this.ending) void this.fail("webrtc"); });
      }
    };
    worker.onerror = () => { if (this.worker === worker && !this.ending) void this.fail("worker"); };
    worker.postMessage({ type: "init", generation: 0, apiBase: this.testing?.apiBase ?? API_BASE });
  }
  rpc(type, fields = {}) {
    this.ensureWorker();
    const id = ++this.rpcId;
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject });
      this.worker.postMessage({ type, id, generation: this.generation, ...fields });
    });
  }
  async inspect(invite) {
    try {
      const result = await this.rpc("inspect", { invite });
      return { hostNick: result.hostNick, wireRate: result.wireRate, roomMode: result.roomMode };
    } catch (error) { if (!this.ending) await this.fail(error.message); throw error; }
  }
  prepareMicrophone(deviceId = null) {
    if (this.prepareTask) return this.prepareTask;
    this.prepareTask = this.prepare(deviceId).finally(() => { this.prepareTask = null; });
    return this.prepareTask;
  }
  async prepare(deviceId) {
    if (this.disposed || this.ending || ["pending", "connecting", "connected"].includes(this.state)) throw new Error("state");
    const life = ++this.lifecycle;
    for (const track of this.stream?.getTracks() ?? []) track.stop();
    this.stream = null;
    if (this.context) { await this.context.close(); this.context = null; }
    let stream, context;
    try {
      // Labels, and so the phone's mic, are known before opening only once the page has the microphone permission.
      let chosen = deviceId || phoneMicForBluetooth(await navigator.mediaDevices.enumerateDevices());
      stream = await openMicrophone(chosen);
      if (!chosen) {
        const phone = phoneMicForBluetooth(await navigator.mediaDevices.enumerateDevices());
        if (phone) {
          stream.getTracks().forEach(t => t.stop());
          await new Promise(resolve => setTimeout(resolve, 500));   // let the phone drop the earbuds' call link
          try { stream = await openMicrophone(phone); chosen = phone; }
          catch { stream = await openMicrophone(null); }
        }
      }
      if (life !== this.lifecycle || this.disposed || this.ending) { stream.getTracks().forEach(t => t.stop()); throw new Error("cancelled"); }
      context = new AudioContext({ latencyHint: "interactive" });
      await context.audioWorklet.addModule(new URL("./audio/worklet.js", import.meta.url));
      await context.resume();
      if (life !== this.lifecycle || this.disposed || this.ending) throw new Error("cancelled");
      const tracks = stream.getAudioTracks();
      if (!tracks.length || tracks[0].readyState !== "live") throw new Error("microphone_missing");
      this.stream = stream; this.context = context;
      this.microphoneId = chosen || tracks[0].getSettings().deviceId || null;
      tracks[0].onended = () => { if (this.stream === stream && !this.ending) void this.fail("microphone_ended"); };
      this.diagnostics({ type: "microphone", trackSettings: tracks[0].getSettings(),
        sampleRate: context.sampleRate, baseLatency: context.baseLatency, outputLatency: context.outputLatency });
      const devices = await navigator.mediaDevices.enumerateDevices();
      if (life !== this.lifecycle || this.ending || this.disposed) throw new Error("cancelled");
      this.setState("ready");
      return devices.filter(d => d.kind === "audioinput");
    } catch (error) {
      stream?.getTracks().forEach(t => t.stop());
      if (context && context.state !== "closed") await context.close();
      if (this.context === context) { this.context = null; this.stream = null; }
      const code = error.message === "cancelled" ? "cancelled" : microphoneCode(error);
      if (code !== "cancelled" && !this.ending) await this.fail(code);
      throw new Error(code);
    }
  }
  async requestJoin(invite, nickname) {
    if (this.joinTask || this.state !== "ready" || !this.stream?.getAudioTracks().some(t => t.readyState === "live")) throw new Error("microphone_required");
    const nick = String(nickname).replace(/[\u0000-\u001f\u007f-\u009f]/gu, "").trim();
    if (!nick || [...nick].length > 40 || new TextEncoder().encode(nick).length > 160) throw new Error("nickname");
    this.joinTask = this.rpc("join", { invite, nick, connectionId: connectionId() });
    try { await this.joinTask; }
    catch (error) { if (!this.ending) await this.fail(error.message); throw error; }
    finally { this.joinTask = null; }
  }
  async accept(m) {
    if (!this.context || !this.stream) throw new Error("microphone_required");
    this.generation = m.generation; this.wireRate = m.wireRate;
    this.setState("connecting");
    const generation = this.generation;
    this.node = new AudioWorkletNode(this.context, "livemix-guest", { numberOfInputs: 1, numberOfOutputs: 1,
      outputChannelCount: [2], channelCount: 2, channelCountMode: "explicit", channelInterpretation: "discrete",
      processorOptions: { generation, wireRate: m.wireRate } });
    this.node.onprocessorerror = () => { if (this.generation === generation && !this.ending) void this.fail("audio"); };
    const audio = new MessageChannel();
    const config = { type: "audioPort", generation, wireRate: m.wireRate, deviceRate: this.context.sampleRate };
    this.node.port.postMessage({ ...config, port: audio.port1 }, [audio.port1]);
    this.worker.postMessage({ ...config, port: audio.port2 }, [audio.port2]);
    this.source = this.context.createMediaStreamSource(this.stream);
    this.source.connect(this.node); this.node.connect(this.context.destination);
    this.testing?.onAudioNode?.(this.node, this.context);
    for (const gain of this.gains) this.node.port.postMessage({ type: "peerGain", generation, participantId: gain.id, gain: gain.gain });
    this.worker.postMessage({ type: "mic", generation, enabled: this.micEnabled });
    this.candidates = []; this.candidateCount = 0; this.sentCandidates = 0; this.endSent = false; this.channelSeen = false;
    const pc = new RTCPeerConnection({ iceServers: this.testing?.iceServers ?? m.iceServers ?? STUN });
    this.pc = pc;
    const current = () => this.pc === pc && this.generation === generation && !this.ending;
    pc.onicecandidate = e => {
      if (!current()) return;
      // The host's ICE (libjuice) has no TCP: a browser's TCP host candidates only use up the 64 per side.
      if (e.candidate && (e.candidate.candidate.split(" ")[2] ?? "").toLowerCase() === "tcp") return;
      if (e.candidate) {
        if (++this.sentCandidates > 64) { void this.fail("ice_candidates"); return; }
        this.localSignal({ type: "candidate", candidate: e.candidate.candidate, sdpMid: e.candidate.sdpMid, sdpMLineIndex: e.candidate.sdpMLineIndex });
      } else if (!this.endSent) { this.endSent = true; this.localSignal({ type: "endOfCandidates" }); }
    };
    pc.onconnectionstatechange = () => {
      if (!current()) return;
      if (pc.connectionState === "failed") void this.fail("ice");
      else if (pc.connectionState === "closed") void this.finish("ended", false);
    };
    pc.ondatachannel = e => {
      if (!current()) { e.channel.close(); return; }
      if (this.channelSeen) { e.channel.close(); void this.fail("channel_config"); return; }
      this.channelSeen = true;
      // Transfer first: no send(), listeners or other channel use beforehand.
      try {
        if (this.testing?.forceRelay) throw new DOMException("test", "DataCloneError");
        this.worker.postMessage({ type: "channel", generation, channel: e.channel }, [e.channel]);
      } catch (error) {
        if (!["DataCloneError", "NotSupportedError", "TypeError"].includes(error.name)) { void this.fail("channel"); return; }
        this.attachRelay(e.channel, generation);
      }
    };
    // 20 s (10/28): a path opened through two routers, or a relay, can take longer than the 10 s that cut off a join the
    // two sides were still checking. The room server keeps a web seat 30 s (WEB_RESERVATION_SEC).
    this.connectTimer = setTimeout(() => { if (current() && this.state !== "connected") void this.fail("connect_timeout"); }, 20000);
  }
  localSignal(signal) { this.worker?.postMessage({ type: "localSignal", generation: this.generation, signal }); }
  async remoteDescription(m) {
    if (!this.pc || this.pc.remoteDescription || !/^m=application\s/m.test(m.sdp) || /^m=(audio|video)\s/m.test(m.sdp)) throw new Error("sdp");
    const pc = this.pc;
    await pc.setRemoteDescription({ type: "offer", sdp: m.sdp });
    if (this.pc !== pc || this.ending) return;
    for (const candidate of this.candidates) await pc.addIceCandidate(candidate);
    this.candidates.length = 0;
    const answer = await pc.createAnswer(); await pc.setLocalDescription(answer);
    if (this.pc === pc && !this.ending) this.localSignal({ type: "answer", sdp: pc.localDescription.sdp });
  }
  async remoteCandidate(signal) {
    if (!this.pc) return;
    let candidate = null;
    if (signal.type === "candidate") {
      if (++this.candidateCount > 64) throw new Error("ice_candidates");
      candidate = { candidate: signal.candidate, sdpMid: signal.sdpMid, sdpMLineIndex: signal.sdpMLineIndex };
    }
    if (this.pc.remoteDescription) await this.pc.addIceCandidate(candidate);
    else { if (this.candidates.length >= 65) throw new Error("ice_candidates"); this.candidates.push(candidate); }
  }
  attachRelay(channel, generation) {
    if (!validChannel(channel)) { channel.close(); void this.fail("channel_config"); return; }
    this.channel = channel;
    const pair = new MessageChannel(); const port = pair.port1; this.relay = port;
    const pool = new TransferPool(2 * audioPackets(this.wireRate), 1200, "packet", generation);
    let dropped = 0;
    const current = () => !this.ending && this.channel === channel && this.generation === generation;
    const status = negotiation => { if (current()) port.postMessage({ type: "transport", generation, bufferedAmount: channel.bufferedAmount, dropped, negotiation }); };
    channel.binaryType = "arraybuffer";
    channel.onopen = () => { if (!validChannel(channel)) void this.fail("channel_config"); else status(channelInfo(channel)); };
    channel.onmessage = e => {
      if (!current()) return;
      if (!(e.data instanceof ArrayBuffer) || e.data.byteLength > 1200) { dropped++; return; }
      const slot = pool.take();
      if (!slot) { dropped++; return; }
      slot.bytes.set(new Uint8Array(e.data));
      Object.assign(slot.message, { bytes: e.data.byteLength, arrivalNs: BigInt(Math.round(performance.now() * 1e6)) });
      port.postMessage(slot.message, slot.transfer);
    };
    channel.onclose = () => { if (current()) void this.finish("ended", false); };
    channel.onerror = e => { if (current()) normalChannelClose(e) ? void this.finish("ended", false) : void this.fail("channel"); };
    port.onmessage = e => {
      const m = e.data;
      if (!current() || m.generation !== generation) return;
      if (m.type === "recycleTransport") { pool.recycle(m.buffer); return; }
      if (m.type !== "packet") return;
      let sent = false;
      if (channel.readyState === "open" && (m.audio ? !expired(m.createdAtMs) &&
          channel.bufferedAmount + m.buffer.byteLength <= audioBudget(this.wireRate) : channel.bufferedAmount < audioBudget(this.wireRate) + 1200)) {
        try { channel.send(m.buffer); sent = true; } catch { /* a closing channel is handled by its event */ }
      }
      if (m.audio) port.postMessage({ type: "recycleTransport", generation, buffer: m.buffer,
        sent, bufferedAmount: channel.bufferedAmount }, [m.buffer]);
      else port.postMessage({ type: "controlSent", generation, sent, revision: m.revision });
    };
    port.start();
    this.worker.postMessage({ type: "transportPort", generation, port: pair.port2 }, [pair.port2]);
    this.relayTimer = setInterval(() => status(), 100);
    if (channel.readyState === "open") channel.onopen();
  }
  setMicEnabled(enabled) {
    this.micEnabled = !!enabled;
    this.worker?.postMessage({ type: "mic", generation: this.generation, enabled: this.micEnabled });
  }
  setPeerVolume(participantId, gain) {
    if (!Number.isInteger(participantId) || participantId < 1 || participantId > 0xffffffff || !Number.isFinite(gain) || gain < 0 || gain > 1)
      throw new RangeError("gain");
    let entry = this.gains.find(v => v.id === participantId);
    if (!entry) { if (this.gains.length === 3) this.gains.shift(); entry = { id: participantId, gain }; this.gains.push(entry); }
    entry.gain = gain;
    this.node?.port.postMessage({ type: "peerGain", generation: this.generation, participantId, gain });
  }
  async fail(code) {
    if (this.ending || this.disposed) return;
    this.callbacks.onError({ code });
    await this.finish("error", true);
  }
  cancel() { return this.finish("ended", true); }
  leave() { return this.finish("ended", true); }
  async finish(state, notifyServer) {
    if (this.finishTask) return this.finishTask;
    this.ending = true; ++this.lifecycle;
    this.finishTask = this.cleanup(state, notifyServer);
    try { await this.finishTask; }
    finally { this.finishTask = null; this.ending = false; }
  }
  async cleanup(state, notifyServer) {
    clearInterval(this.relayTimer); clearTimeout(this.connectTimer);
    for (const track of this.stream?.getTracks() ?? []) { track.onended = null; track.stop(); }
    this.stream = null;
    if (this.node) { this.node.port.postMessage({ type: "close", generation: this.generation }); this.node.port.close(); this.node.disconnect(); }
    this.source?.disconnect(); this.source = this.node = null;
    if (this.channel) { this.channel.onclose = this.channel.onerror = this.channel.onmessage = this.channel.onopen = null; this.channel.close(); this.channel = null; }
    if (this.pc) { this.pc.ondatachannel = this.pc.onicecandidate = this.pc.onconnectionstatechange = null; this.pc.close(); this.pc = null; }
    this.relay?.close(); this.relay = null;
    if (this.context && this.context.state !== "closed") await this.context.close();
    this.context = null;
    if (this.joinTask) { try { await this.joinTask; } catch { /* join error has no reservation */ } }
    if (notifyServer && this.worker) {
      try { await this.rpc("leave"); }
      catch (error) { this.callbacks.onError({ code: error.message }); }
    }
    if (this.worker) {
      this.worker.postMessage({ type: "close", generation: this.generation });
      this.worker.terminate(); this.worker = null;
    }
    for (const request of this.pending.values()) request.reject(new Error("cancelled"));
    this.pending.clear(); this.setState(state);
  }
  async dispose() {
    if (this.disposed) return;
    await this.finish("ended", true); this.disposed = true;
  }
}
