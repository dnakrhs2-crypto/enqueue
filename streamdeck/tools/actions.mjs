// Manifest, localization and placeholder assets use the same action catalogue.
export const actions = [
  { id: "mic", name: "action", tooltip: "tooltip", states: ["off", "on"] },
  { id: "all-mics", name: "allMics", tooltip: "allMicsTooltip", states: ["allOff", "allOn"] },
  { id: "mic-mute-group", name: "micMuteGroup", tooltip: "micMuteGroupTooltip", states: ["unmuteState", "muteState"] },
  { id: "fx-mute-group", name: "fxMuteGroup", tooltip: "fxMuteGroupTooltip", states: ["unmuteState", "muteState"] },
  { id: "plugin-group", name: "pluginGroup", tooltip: "pluginGroupTooltip", states: ["off", "on"] },
  { id: "fx-send", name: "fxSend", tooltip: "fxSendTooltip", states: ["fxSend"] },
  { id: "fx-send-step", name: "fxSendStep", tooltip: "fxSendStepTooltip", states: ["fxSendStep"] },
  { id: "status", name: "statusAction", tooltip: "statusTooltip", states: ["disconnected", "connected"] }
];
export const triggers = strings => ({ Push: strings.pushTrigger, Rotate: strings.rotateTrigger, Touch: strings.touchTrigger, LongTouch: strings.longTouchTrigger });
