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
window.r11 = {
  calls,
  config,
  roster(members = config.members ?? defaultMembers, selfId = config.selfId ?? 3) {
    callbacks.onRoster({ members, selfId });
  },
  state(state) { callbacks.onState({ state, generation: 1 }); },
  meter(peak) { callbacks.onMeter({ peak }); },
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
      ready = true;
      callbacks.onState({ state: "ready", generation: 0 });
      return config.devices ?? [
        { deviceId: "phone", kind: "audioinput", label: "휴대폰 마이크" },
        { deviceId: "shure", kind: "audioinput", label: "마이크(Shure MV7)" },
      ];
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
