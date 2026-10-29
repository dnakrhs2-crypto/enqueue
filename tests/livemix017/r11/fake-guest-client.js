// Served only by the Playwright route. Production has no global/query test switch.
export { parseInvite } from "./guest-client.js?r11-real-parse";

const config = window.r11Fixture ?? {};
const calls = [];
let callbacks;
const defaultMembers = [
  { participantId: 1, nick: "하람", kind: "native", state: "connected" },
  { participantId: 2, nick: "윤슬", kind: "native", state: "connected" },
  { participantId: 3, nick: "하늘", kind: "web", state: "connected" },
];
const defaultDevices = [
  { deviceId: "phone", kind: "audioinput", label: "휴대폰 마이크" },
  { deviceId: "shure", kind: "audioinput", label: "마이크(Shure MV7)" },
];
window.r11 = {
  calls,
  config,
  roster(members = config.members ?? defaultMembers, selfId = config.selfId ?? 3) {
    callbacks.onRoster({ members, selfId });
  },
  state(state) { callbacks.onState({ state, generation: 1 }); },
  meter(peak) { callbacks.onMeter({ peak }); },
  microphoneEnded() { callbacks.onMicrophoneError({ code: "microphone_ended" }); this.meter(0); },
  error(code) { callbacks.onError({ code }); callbacks.onState({ state: "error", generation: 1 }); },
  accept() {
    this.state("connecting");
    this.roster();
    this.state("connected");
    this.meter(.46);
  },
};

export function createGuestClient(handlers) {
  callbacks = handlers;
  calls.push({ method: "create" });
  callbacks.onState({ state: "idle", generation: 0 });
  let ready = false;
  return {
    async inspect(invite) {
      calls.push({ method: "inspect", code: invite.code, generation: invite.inviteGeneration });
      if (config.inspectError) {
        window.r11.error(config.inspectError);
        throw new Error(config.inspectError);
      }
      return { hostNick: config.hostNick ?? "하람", wireRate: 48000, roomMode: "v2" };
    },
    async prepareMicrophone(deviceId = null) {
      calls.push({ method: "prepareMicrophone", deviceId });
      if (config.microphoneError) {
        window.r11.error(config.microphoneError);
        throw new Error(config.microphoneError);
      }
      // A microphone another program holds: the real client lists the named inputs instead of ending.
      if ((config.heldMicrophones ?? []).includes(deviceId)) {
        ready = false;
        throw Object.assign(new Error("microphone_busy"), { code: "microphone_busy", devices: config.devices });
      }
      ready = true;
      callbacks.onState({ state: "ready", generation: 0 });
      return config.devices || defaultDevices;
    },
    async requestJoin(invite, nickname) {
      if (!ready) throw new Error("microphone_required");
      calls.push({ method: "requestJoin", nickname, code: invite.code });
      if (config.joinError) {
        window.r11.error(config.joinError);
        throw new Error(config.joinError);
      }
      callbacks.onState({ state: "pending", generation: 0 });
    },
    async listMicrophones() {
      calls.push({ method: "listMicrophones" });
      return (config.devices || defaultDevices).filter(device => device.kind === "audioinput" && device.label);
    },
    async switchMicrophone(deviceId) {
      calls.push({ method: "switchMicrophone", deviceId });
      callbacks.onMeter({ peak: 0 });
      if (config.pauseSwitch) await new Promise(resolve => { window.r11.completeSwitch = resolve; });
      if (config.switchErrors?.[deviceId]) {
        const code = config.switchErrors[deviceId];
        throw Object.assign(new Error(code), { code });
      }
      callbacks.onMeter({ peak: .46 });
    },
    setMicEnabled(enabled) { calls.push({ method: "setMicEnabled", enabled }); },
    setPeerVolume(participantId, gain) {
      calls.push({ method: "setPeerVolume", participantId, gain,
        rowVisible: !!document.querySelector(`[data-participant-id="${participantId}"]`) });
    },
    async cancel() { calls.push({ method: "cancel" }); ready = false; window.r11.state("ended"); },
    async leave() { calls.push({ method: "leave" }); ready = false; window.r11.state("ended"); },
    async dispose() { calls.push({ method: "dispose" }); ready = false; window.r11.state("ended"); },
  };
}
