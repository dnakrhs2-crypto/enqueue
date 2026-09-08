/* PI owns only its host WebSocket. No Node APIs, LiveMix socket or discovery access. */
(() => {
  "use strict";
  const byId = id => document.getElementById(id);
  let socket, context, uuid, kind = "mic", settings = {}, channels = [], fx = [], groupIndices = [], counts;
  let request = 0, requestId = "", sequence = -1, session, revision = -1, ready = false;
  let t = window.LiveMixStrings.en;
  const isMute = () => kind.endsWith("mute-group");
  const isSendStep = () => kind === "fx-send-step";
  function send(event, payload) { if (socket?.readyState === WebSocket.OPEN) socket.send(JSON.stringify({ event, context: uuid, payload })); }
  function option(value, text) { const el = document.createElement("option"); el.value = String(value); el.textContent = text; return el; }
  function choices(id, values) { const el = byId(id); el.replaceChildren(); for (const [value, label] of values) el.append(option(value, label)); }
  function showSettings() {
    byId("mode").value = (isSendStep() ? ["up", "down", "set"] : isMute() ? ["mute", "unmute"] : ["on", "off"]).includes(settings.mode) ? settings.mode : isSendStep() ? "up" : "toggle";
    byId("fallback").checked = settings.nameFallback !== false;
    byId("short-title").value = typeof settings.shortTitle === "string" ? settings.shortTitle : "";
    byId("step").value = String((isSendStep() ? [1, 5, 10] : [1, 5]).includes(settings.stepPercent) ? settings.stepPercent : isSendStep() ? 5 : 1);
    byId("target").value = String(Number.isInteger(settings.targetPercent) && settings.targetPercent >= 0 && settings.targetPercent <= 100 ? settings.targetPercent : 50);
    byId("target-row").hidden = !isSendStep() || byId("mode").value !== "set";
    byId("step-row").hidden = kind !== "fx-send" && (!isSendStep() || byId("mode").value === "set");
    byId("press").value = settings.pressMode === "none" ? "none" : "pre-post";
    byId("display").value = ["session", "audio"].includes(settings.display) ? settings.display : "connection";
  }
  function updateSelect(id, items, selectedId, selectedName, placeholder) {
    const select = byId(id); select.replaceChildren(); select.disabled = !ready;
    if (!items.some(c => c.id === selectedId)) select.append(option(selectedId || "", selectedName || placeholder));
    for (const item of items) {
      const duplicate = items.filter(c => c.name === item.name).length > 1;
      select.append(option(item.id, item.name + (duplicate ? " · " + item.id.slice(0, 8) : "")));
    }
    select.value = selectedId || "";
  }
  function updateLists() {
    updateSelect("channel", channels, settings.channelId, settings.channelName, t.choose);
    updateSelect("fx", fx, settings.fxId, settings.fxName, t.chooseFx);
    const group = byId("group"), index = settings.groupIndex || 1;
    const indices = channels.find(c => c.id === settings.channelId)?.groupIndices || groupIndices;
    group.replaceChildren(); group.disabled = !ready;
    if (!indices.includes(index)) {
      const missing = option(index, t.group + " " + index + (ready ? " · " + t.missingGroup : ""));
      missing.disabled = true; group.append(missing);
    }
    for (const n of indices) group.append(option(n, t.group + " " + n));
    group.value = String(index);
    if (isMute()) byId("note").textContent = t.membershipNote + "\n" + t.targets.replace("{count}", String(ready && counts ? counts[kind === "mic-mute-group" ? "mic" : "fx"] : "—"));
  }
  function save(patch) {
    settings = { ...settings, settingsVersion: 1, ...patch };
    send("setSettings", settings);
  }
  byId("channel").addEventListener("change", () => {
    const selected = channels.find(c => c.id === byId("channel").value);
    if (selected) { save({ channelId: selected.id, channelName: selected.name }); groupIndices = selected.groupIndices || []; updateLists(); }
  });
  byId("fx").addEventListener("change", () => {
    const selected = fx.find(c => c.id === byId("fx").value);
    if (selected) save({ fxId: selected.id, fxName: selected.name });
  });
  byId("group").addEventListener("change", () => save({ groupIndex: Number(byId("group").value) }));
  byId("mode").addEventListener("change", () => { save({ mode: byId("mode").value }); showSettings(); });
  byId("step").addEventListener("change", () => save({ stepPercent: Number(byId("step").value) }));
  byId("target").addEventListener("change", () => {
    const value = byId("target").value, amount = Number(value);
    if (value.trim() && Number.isFinite(amount)) save({ targetPercent: Math.max(0, Math.min(100, Math.round(amount))) });
    showSettings();
  });
  byId("press").addEventListener("change", () => save({ pressMode: byId("press").value }));
  byId("display").addEventListener("change", () => save({ display: byId("display").value }));
  byId("fallback").addEventListener("change", () => save({ nameFallback: byId("fallback").checked }));
  byId("short-title").addEventListener("input", () => save({ shortTitle: byId("short-title").value }));
  window.connectElgatoStreamDeckSocket = (port, propertyInspectorUUID, registerEvent, infoString, actionString) => {
    const info = JSON.parse(infoString), action = JSON.parse(actionString);
    uuid = propertyInspectorUUID; context = action.context; settings = action.payload.settings || {};
    kind = action.action.split(".").at(-1);
    t = window.LiveMixStrings[info.application.language === "ko" ? "ko" : "en"];
    document.documentElement.lang = info.application.language === "ko" ? "ko" : "en";
    for (const [id, key] of [["channel", "microphone"], ["fx", "fxChannel"], ["group", "group"], ["mode", "mode"], ["step", "step"], ["target", "targetValue"], ["press", "press"], ["display", "display"], ["fallback", "fallback"], ["title", "shortTitle"]]) byId(id + "-label").textContent = t[key];
    const bound = ["mic", "plugin-group", "fx-send", "fx-send-step"].includes(kind);
    for (const [id, visible] of [["channel", bound], ["fx", kind === "fx-send" || isSendStep()], ["group", kind === "plugin-group"],
      ["mode", !["fx-send", "status"].includes(kind)], ["step", kind === "fx-send"], ["press", kind === "fx-send"],
      ["display", kind === "status"], ["fallback", bound], ["title", !["fx-send", "fx-send-step", "status"].includes(kind)]]) byId(id + "-row").hidden = !visible;
    choices("mode", isSendStep() ? [["up", t.increase], ["down", t.decrease], ["set", t.setValue]]
      : [["toggle", t.toggle], ...(isMute() ? [["mute", t.mute], ["unmute", t.unmute]] : [["on", kind === "all-mics" ? t.setAllOn : t.on], ["off", kind === "all-mics" ? t.setAllOff : t.off]])]);
    choices("step", (isSendStep() ? [1, 5, 10] : [1, 5]).map(value => [value, value + "%"]));
    choices("press", [["pre-post", t.prePost], ["none", t.pressNone]]);
    choices("display", [["connection", t.connection], ["session", t.session], ["audio", t.audio]]);
    byId("note").hidden = !isMute() && !["plugin-group", "fx-send"].includes(kind);
    byId("note").textContent = kind === "plugin-group" ? t.groupNote : kind === "fx-send" ? t.dialNote : "";
    byId("status").textContent = t.offlineHelp;
    showSettings(); updateLists();
    socket = new WebSocket("ws://127.0.0.1:" + port);
    socket.onopen = () => {
      socket.send(JSON.stringify({ event: registerEvent, uuid }));
      requestId = "pi" + ++request;
      socket.send(JSON.stringify({ event: "sendToPlugin", action: action.action, context: uuid, payload: { op: "getOptions", requestId } }));
    };
    socket.onmessage = event => {
      const message = JSON.parse(event.data);
      if (message.context !== uuid && message.context !== context) return;
      if (message.event === "didReceiveSettings") { settings = message.payload.settings; showSettings(); updateLists(); return; }
      const p = message.payload;
      if (message.event !== "sendToPropertyInspector" || !p || p.op !== "options" || p.context !== context || p.requestId !== requestId || !Number.isSafeInteger(p.sequence) || p.sequence <= sequence) return;
      const nextSession = p.instanceId + "/" + p.sessionId;
      if (p.connection === "ready" && (!Number.isSafeInteger(p.revision) || (session === nextSession && p.revision < revision))) return;
      sequence = p.sequence; ready = p.connection === "ready";
      if (ready) { session = nextSession; revision = p.revision; }
      channels = ready && Array.isArray(p.channels) ? p.channels : [];
      fx = ready && Array.isArray(p.fx) ? p.fx : [];
      groupIndices = ready && Array.isArray(p.groupIndices) ? p.groupIndices : [];
      counts = ready ? p.muteGroupCounts : undefined;
      byId("status").textContent = p.message;
      updateLists();
    };
    socket.onclose = () => { ready = false; channels = []; fx = []; groupIndices = []; updateLists(); byId("status").textContent = t.offlineHelp; };
  };
  window.addEventListener("beforeunload", () => socket?.close());
})();
