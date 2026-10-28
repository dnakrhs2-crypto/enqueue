import { Signaling, iceServersFor } from "../signaling.js";
import { CAPABILITIES, Kind, readPacket, readRoster, rosterAck, controlPacket, validDownlink,
  decodeAudio, encodeAudio, sessionValue, validRate } from "./packet.js";
import { TransferPool, audioPackets, audioBudget, expired, validChannel, channelInfo, normalChannelClose } from "./buffers.js";

export class GuestWorker {
  constructor(send, options = {}) {
    this.send = send; this.generation = 0; this.closed = false;
    this.channel = null; this.transport = null; this.audio = null;
    this.members = []; this.room = null; this.revision = 0; this.activeRevision = 0;
    this.controls = []; this.buffered = 0; this.mic = true;
    this.metrics = { received: 0, sent: 0, invalid: 0, dropped: 0, playbackDropped: 0, transportDropped: 0, bufferedBytes: 0 };
    this.signaling = new Signaling({ ...options, onEvent: e => this.event(e), onError: code => this.fail(code) });
    this.timer = setInterval(() => this.flushControls(), 5);
  }
  post(type, fields = {}) { this.send({ type, generation: this.generation, ...fields }); }
  fail(code) {
    if (this.closed) return;
    if (code === "closed" || code === "expired") { this.post("state", { state: "ended" }); return; }
    this.post("error", { code });
  }
  async command(m) {
    try {
      let result;
      if (m.type === "inspect") result = await this.signaling.inspect(m.invite);
      else if (m.type === "join") {
        result = await this.signaling.join(m.invite, m.nick, m.connectionId, CAPABILITIES);
        this.post("state", { state: "pending" });
      } else if (m.type === "leave") { await this.signaling.leave(); result = null; }
      else return;
      this.post("reply", { id: m.id, result: m.type === "inspect" ? result : null });
      if (m.type === "join") this.signaling.startPolling();
    } catch (error) { this.post("reply", { id: m.id, code: error.code ?? "network" }); }
  }
  configureConnection(d) {
    if (!validRate(d.room.wireRate) || d.room.roomMode !== "v2" || !d.participantId || !d.streamEpoch ||
        !d.room.members.some(m => m.participantId === d.participantId && m.kind === "web")) throw new Error("incompatible");
    this.generation = d.connectionGeneration; this.selfId = d.participantId; this.epoch = d.streamEpoch;
    this.room = d.room; this.wireRate = d.room.wireRate; this.session = sessionValue(d.room.session);
    this.captureTransport = new TransferPool(audioPackets(this.wireRate), 288, "packet", this.generation);
    // A stall up to the playout's 600 ms ring arrives as one burst of leases before any is recycled.
    this.playbackPool = new TransferPool(2 * Math.ceil(this.wireRate * 0.6 / 120), 960, "playback", this.generation);
    this.header = { session: this.session, origin: this.selfId, linkSender: this.selfId,
      streamEpoch: this.epoch, rosterRevision: 0, seq: 0, firstFrame: 0n };
  }
  event(event) {
    const d = event.data;
    if (event.type === "accepted") {
      this.configureConnection(d);
      // Tokens stay in Signaling, never in snapshots, diagnostics or main messages. The relay credentials are not
      // connection tokens: the main thread needs them to build the RTCPeerConnection.
      this.post("accepted", { selfId: this.selfId, streamEpoch: this.epoch, wireRate: this.wireRate,
        iceServers: iceServersFor(d.iceServers) });
    } else if (event.type === "room") this.room = d.room;
    else if (event.type === "signal" && d.connectionGeneration === this.generation && d.from === "host") {
      if (d.signal.type === "offer") this.post("remoteDescription", { sdp: d.signal.sdp });
      else if (d.signal.type === "candidate" || d.signal.type === "endOfCandidates") this.post("remoteCandidate", { signal: d.signal });
    } else if (event.type === "ended" || (event.type === "requestState" && ["rejected", "cancelled", "expired", "closed", "invalidated"].includes(d.state))) {
      this.post("state", { state: (d.reason ?? d.state) === "rejected" ? "rejected" : "ended" });
      this.signaling.close();
    }
  }
  message(m) {
    if (this.closed) return;
    if (["inspect", "join", "leave"].includes(m.type)) { void this.command(m); return; }
    if (m.generation !== this.generation) return;
    if (m.type === "audioPort") {
      this.deviceRate = m.deviceRate; this.audio = m.port;
      this.audio.onmessage = e => this.audioMessage(e.data); this.audio.start?.();
      this.audio.postMessage({ type: "mic", generation: this.generation, enabled: this.mic });
    } else if (m.type === "channel") this.attachChannel(m.channel);
    else if (m.type === "transportPort") {
      this.transport = m.port; this.transport.onmessage = e => this.transportMessage(e.data); this.transport.start?.();
    } else if (m.type === "localSignal") {
      void this.signaling.send(m.signal, this.generation).catch(error => this.fail(error.code));
    } else if (m.type === "mic") {
      this.mic = !!m.enabled; this.audio?.postMessage({ ...m, enabled: this.mic });
    } else if (m.type === "close") this.close();
  }
  attachChannel(channel) {
    this.channel = channel;
    if (!validChannel(channel)) { this.fail("channel_config"); return; }
    channel.binaryType = "arraybuffer";
    channel.onmessage = e => this.receive(e.data, BigInt(Math.round(performance.now() * 1e6)));
    channel.onopen = () => {
      if (!validChannel(channel)) { this.fail("channel_config"); return; }
      this.post("transport", { path: "worker", negotiation: channelInfo(channel) }); this.flushControls();
    };
    channel.onerror = e => normalChannelClose(e) ? this.post("state", { state: "ended" }) : this.fail("channel");
    channel.onclose = () => this.post("state", { state: "ended" });
    if (channel.readyState === "open") channel.onopen();
  }
  transportMessage(m) {
    if (m.generation !== this.generation || this.closed) return;
    if (m.type === "packet") {
      this.receive(new Uint8Array(m.buffer, 0, m.bytes), m.arrivalNs);
      this.transport.postMessage({ type: "recycleTransport", generation: this.generation, direction: "receive", buffer: m.buffer }, [m.buffer]);
    } else if (m.type === "recycleTransport") {
      this.captureTransport.recycle(m.buffer); this.buffered = m.bufferedAmount;
      if (m.sent) this.metrics.sent++; else this.metrics.dropped++;
    } else if (m.type === "controlSent") {
      if (m.sent && m.revision) this.activate(m.revision);
    } else if (m.type === "transport") {
      this.buffered = m.bufferedAmount ?? 0;
      this.metrics.transportDropped = m.dropped ?? 0;
      if (m.negotiation) this.post("transport", { path: "main", negotiation: m.negotiation });
    } else if (m.type === "ended") this.post("state", { state: "ended" });
    else if (m.type === "error") this.fail(m.code);
  }
  audioMessage(m) {
    if (m.generation !== this.generation || this.closed) return;
    if (m.type === "capture") {
      try { this.capture(m); }
      finally { this.audio.postMessage({ type: "recycle", generation: this.generation, pool: "capture", buffer: m.buffer }, [m.buffer]); }
    } else if (m.type === "recycle" && m.pool === "playback") this.playbackPool.recycle(m.buffer);
    else if (m.type === "stats") {
      this.metrics.bufferedBytes = this.channel?.bufferedAmount ?? this.buffered;
      this.post("meter", { peak: m.peak });
      this.post("metrics", { audio: m, transport: { ...this.metrics } });
    }
  }
  capture(m) {
    if (!this.activeRevision || m.streamEpoch !== this.epoch || m.frames !== 120 || expired(m.createdAtMs)) { this.metrics.dropped++; return; }
    const waiting = this.transport ? this.captureTransport.borrowed() * 288 : 0;
    if ((this.channel?.bufferedAmount ?? this.buffered) + waiting + 288 > audioBudget(this.wireRate)) { this.metrics.dropped++; return; }
    const slot = this.captureTransport.take();
    if (!slot) { this.metrics.dropped++; return; }
    this.header.firstFrame = m.firstFrame;
    this.header.seq = Number((m.firstFrame / 120n) & 0xffffffffn);
    this.header.streamEpoch = this.epoch; this.header.rosterRevision = this.activeRevision;
    encodeAudio(this.header, new Float32Array(m.buffer), true, slot.buffer);
    if (this.channel?.readyState === "open") {
      try { this.channel.send(slot.buffer); this.metrics.sent++; }
      catch { this.metrics.dropped++; }
      slot.busy = false;
    } else if (this.transport) {
      Object.assign(slot.message, { audio: true, createdAtMs: m.createdAtMs });
      this.transport.postMessage(slot.message, slot.transfer);
    } else { slot.busy = false; this.metrics.dropped++; }
  }
  receive(data, arrivalNs) {
    if (this.closed || !this.audio || !this.room) return;
    try {
      const h = readPacket(data, this.session);
      if (h.kind === Kind.roster) {
        if (h.rosterRevision < this.revision) return;
        const roster = readRoster(h, { selfId: this.selfId, wireRate: this.wireRate,
          allowedIds: this.room.members.map(m => m.participantId), previous: this.members });
        if (h.rosterRevision > this.revision) {
          const self = roster.members.find(m => m.participantId === this.selfId);
          if (self.streamEpoch !== this.epoch) throw new Error("packet");
          this.members = roster.members; this.revision = h.rosterRevision;
          this.audio.postMessage({ type: "people", generation: this.generation, rosterRevision: this.revision,
            members: this.members.filter(m => m.participantId !== this.selfId).map(m => ({ participantId: m.participantId, streamEpoch: m.streamEpoch })) });
          this.post("roster", { members: this.members.map(({ participantId, nick, kind, state }) => ({ participantId, nick, kind, state })), selfId: this.selfId });
        }
        this.control(rosterAck(h, this.selfId), h.rosterRevision);
      } else if (validDownlink(h, this.selfId, this.revision, this.members)) {
        const slot = this.playbackPool.take();
        if (!slot) { this.metrics.dropped++; this.metrics.playbackDropped++; return; }
        decodeAudio(h, slot.floats);
        Object.assign(slot.message, { origin: h.origin, streamEpoch: h.streamEpoch, seq: h.seq,
          firstFrame: h.firstFrame, frames: 120, arrivalNs });
        this.audio.postMessage(slot.message, slot.transfer); this.metrics.received++;
      } else if (h.kind === Kind.ping && h.origin === 1) {
        this.control(controlPacket({ ...h, origin: this.selfId, linkSender: this.selfId }, Kind.pong, h.bytes.subarray(48)));
      } else if (h.kind === Kind.bye && h.origin === 1) this.post("state", { state: "ended" });
      else this.metrics.invalid++;
    } catch { this.metrics.invalid++; }
  }
  control(buffer, revision = 0) {
    // Only bounded, independently prioritised control traffic. Roster retransmission
    // retries a lost ACK; no audio is ever placed on this retry queue.
    if (revision) this.controls = this.controls.filter(c => !c.revision);
    if (this.controls.length < 8) this.controls.push({ buffer, revision });
    this.flushControls();
  }
  flushControls() {
    if (this.closed) return;
    while (this.controls.length) {
      const item = this.controls[0];
      if (this.channel?.readyState === "open") {
        if (this.channel.bufferedAmount > audioBudget(this.wireRate) + 1200) return;
        try { this.channel.send(item.buffer); }
        catch { return; }
        this.controls.shift(); if (item.revision) this.activate(item.revision);
      } else if (this.transport) {
        this.controls.shift();
        this.transport.postMessage({ type: "packet", generation: this.generation, audio: false,
          revision: item.revision, buffer: item.buffer }, [item.buffer]);
      } else return;
    }
  }
  activate(revision) {
    if (revision > this.revision || revision <= this.activeRevision) return;
    const first = !this.activeRevision; this.activeRevision = revision;
    this.audio.postMessage({ type: "configure", generation: this.generation, wireRate: this.wireRate,
      deviceRate: this.deviceRate, streamEpoch: this.epoch, rosterRevision: revision });
    if (first) this.post("state", { state: "connected" });
  }
  close() {
    this.closed = true; clearInterval(this.timer); this.signaling.close();
    this.audio?.postMessage({ type: "close", generation: this.generation });
    this.audio?.close(); this.transport?.close();
    if (this.channel) {
      this.channel.onopen = this.channel.onmessage = this.channel.onerror = this.channel.onclose = null;
      this.channel.close();
    }
  }
}

if (typeof WorkerGlobalScope !== "undefined" && globalThis instanceof WorkerGlobalScope) {
  let engine;
  self.onmessage = e => {
    const m = e.data;
    if (m.type === "init" && !engine) engine = new GuestWorker(value => self.postMessage(value), { apiBase: m.apiBase });
    else engine?.message(m);
  };
}
