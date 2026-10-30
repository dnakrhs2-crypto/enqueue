import type { Projection, Channel, Fx } from "./protocol.js";
import type { MuteCause } from "../ui/artwork.js";

/** Why a mic / FX is silent: the whole-mute switch first, then its mute group (LiveMix's card lamp uses the same order). */
export const micMuteCause = (state: Projection, channel: Channel): MuteCause | undefined =>
  state.muteAll.mic ? "all" : state.muteGroups.mic && channel.muteGroup ? "group" : undefined;
export const fxMuteCause = (state: Projection, fx: Fx): MuteCause | undefined =>
  state.muteAll.fx ? "all" : state.muteGroups.fx && fx.muteGroup ? "group" : undefined;
/** A send is silent when its mic or its FX is. */
export const sendMuteCause = (state: Projection, channel: Channel, fx: Fx): MuteCause | undefined => {
  const mic = micMuteCause(state, channel), ret = fxMuteCause(state, fx);
  return mic === "all" || ret === "all" ? "all" : mic ?? ret;
};
export const micMuted = (state: Projection, channel: Channel): boolean => micMuteCause(state, channel) !== undefined;
export const fxMuted = (state: Projection, fx: Fx): boolean => fxMuteCause(state, fx) !== undefined;
