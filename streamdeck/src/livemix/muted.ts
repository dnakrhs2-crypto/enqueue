import type { Projection, Channel, Fx } from "./protocol.js";

export const micMuted = (state: Projection, channel: Channel): boolean => state.muteAll.mic || (state.muteGroups.mic && channel.muteGroup);
export const fxMuted = (state: Projection, fx: Fx): boolean => state.muteAll.fx || (state.muteGroups.fx && fx.muteGroup);
