/* PI owns only its host WebSocket. No Node APIs, LiveMix socket or discovery access. */
(() => {
  "use strict";
  const byId = id => document.getElementById(id);
  let socket, context, uuid, settings = {}, channels = [], request = 0, requestId = "", sequence = -1, session, revision = -1;
  let t = window.LiveMixStrings.en;
  function send(event, payload) { if (socket?.readyState === WebSocket.OPEN) socket.send(JSON.stringify({ event, context: uuid, payload })); }
  function option(value, text) { const el = document.createElement("option"); el.value = value; el.textContent = text; return el; }
  function showSettings() {
    byId("mode").value = ["on", "off"].includes(settings.mode) ? settings.mode : "toggle";
    byId("fallback").checked = settings.nameFallback !== false;
    byId("short-title").value = typeof settings.shortTitle === "string" ? settings.shortTitle : "";
  }
  function updateChannels(ready) {
    const select = byId("channel"); select.replaceChildren(); select.disabled = !ready;
    if (!channels.some(c => c.id === settings.channelId)) select.append(option(settings.channelId || "", settings.channelName || t.choose));
    for (const channel of channels) {
      const duplicate = channels.filter(c => c.name === channel.name).length > 1;
      select.append(option(channel.id, channel.name + (duplicate ? ` · ${channel.id.slice(0, 8)}` : "")));
    }
    select.value = settings.channelId || "";
  }
  function save(patch) {
    settings = { ...settings, settingsVersion: 1, ...patch };
    send("setSettings", settings);
  }
  byId("channel").addEventListener("change", () => {
    const selected = channels.find(c => c.id === byId("channel").value);
    if (selected) save({ channelId: selected.id, channelName: selected.name });
  });
  byId("mode").addEventListener("change", () => save({ mode: byId("mode").value }));
  byId("fallback").addEventListener("change", () => save({ nameFallback: byId("fallback").checked }));
  byId("short-title").addEventListener("input", () => save({ shortTitle: byId("short-title").value }));
  window.connectElgatoStreamDeckSocket = (port, propertyInspectorUUID, registerEvent, infoString, actionString) => {
    const info = JSON.parse(infoString), action = JSON.parse(actionString);
    uuid = propertyInspectorUUID; context = action.context; settings = action.payload.settings || {};
    t = window.LiveMixStrings[info.application.language === "ko" ? "ko" : "en"];
    document.documentElement.lang = info.application.language === "ko" ? "ko" : "en";
    byId("channel-label").textContent = t.microphone; byId("mode-label").textContent = t.mode;
    byId("fallback-label").textContent = t.fallback; byId("title-label").textContent = t.shortTitle;
    byId("mode").options[0].textContent = t.toggle; byId("status").textContent = t.offlineHelp;
    showSettings(); updateChannels(false);
    socket = new WebSocket(`ws://127.0.0.1:${port}`);
    socket.onopen = () => {
      socket.send(JSON.stringify({ event: registerEvent, uuid }));
      requestId = `pi${++request}`;
      socket.send(JSON.stringify({ event: "sendToPlugin", action: action.action, context: uuid, payload: { op: "getOptions", requestId } }));
    };
    socket.onmessage = event => {
      const message = JSON.parse(event.data);
      if (message.context !== uuid && message.context !== context) return;
      if (message.event === "didReceiveSettings") { settings = message.payload.settings; showSettings(); updateChannels(!byId("channel").disabled); return; }
      const p = message.payload;
      if (message.event !== "sendToPropertyInspector" || !p || p.op !== "options" || p.context !== context || p.requestId !== requestId || !Number.isSafeInteger(p.sequence) || p.sequence <= sequence) return;
      const nextSession = `${p.instanceId}/${p.sessionId}`;
      if (p.connection === "ready" && (!Number.isSafeInteger(p.revision) || (session === nextSession && p.revision < revision))) return;
      sequence = p.sequence;
      if (p.connection === "ready") { session = nextSession; revision = p.revision; }
      channels = p.connection === "ready" && Array.isArray(p.channels) ? p.channels : [];
      byId("status").textContent = p.message;
      updateChannels(p.connection === "ready");
    };
    socket.onclose = () => { channels = []; updateChannels(false); byId("status").textContent = t.offlineHelp; };
  };
  window.addEventListener("beforeunload", () => socket?.close());
})();
