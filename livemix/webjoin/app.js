import { parseInvite, createGuestClient } from "./guest-client.js";

export const ERROR_TEXT = "초대 주소가 바뀌었거나 방이 닫혔습니다.";
export const NICKNAME_KEY = "livemix.webjoin.nickname";
export const VOLUME_PREFIX = "livemix.webjoin.volume.";

export function cleanNickname(value) {
  return String(value).replace(/[\u0000-\u001f\u007f-\u009f]/gu, "").trim();
}

export function validNickname(value) {
  const nick = cleanNickname(value);
  return nick.length > 0 && [...nick].length <= 40 && new TextEncoder().encode(nick).length <= 160;
}

export function createPreferences(storage = () => window.localStorage) {
  // Getting localStorage itself can throw, as can either operation.
  function read(key) { try { return storage().getItem(key); } catch { return null; } }
  function write(key, value) { try { storage().setItem(key, value); } catch { /* Device memory is optional. */ } }
  return {
    nickname() { const value = read(NICKNAME_KEY); return value && validNickname(value) ? cleanNickname(value) : ""; },
    rememberNickname(value) { write(NICKNAME_KEY, cleanNickname(value)); },
    volume(nick) {
      const value = read(VOLUME_PREFIX + nick);
      return /^(0|[1-9][0-9]?|100)$/.test(value ?? "") ? Number(value) : 100;
    },
    rememberVolume(nick, value) { write(VOLUME_PREFIX + nick, String(value)); },
  };
}

// Only failures people hit in normal use get their own sentence; everything else keeps the §10 sentence.
// Never display Error.message, transport data or a server-provided explanation.
const ERROR_GROUPS = [
  ["마이크 권한이 없습니다.", ["microphone_denied"]],
  ["마이크를 쓸 수 없습니다.", ["microphone_missing", "microphone_busy", "microphone_constraints",
    "microphone", "microphone_ended", "microphone_required"]],
  ["방이 꽉 찼습니다.", ["full", "web_full"]],
  ["방장이 거절했습니다.", ["rejected"]],
  ["연결하지 못했습니다.", ["ice", "ice_candidates", "connect_timeout", "channel", "channel_config", "webrtc", "sdp",
    "network", "timeout"]],
];
export function errorText({ code } = {}) {
  return ERROR_GROUPS.find(([, codes]) => codes.includes(code))?.[0] ?? ERROR_TEXT;
}

export function startApp(doc = document, win = window) {
  const $ = id => doc.getElementById(id);
  const preferences = createPreferences(() => win.localStorage);
  const app = $("app"), title = $("room-title"), entry = $("entry");
  const nickname = $("nickname"), join = $("join"), microphone = $("microphone");
  const options = $("microphone-options"), people = $("people");
  const micToggle = $("mic-toggle"), meter = $("mic-meter");
  const rows = new Map();
  let client, invite, devices = [], deviceId = null, activeDevice = 0;
  let prepared = false, busy = false, micEnabled = true, localEnding = false;
  let screen = "loading";

  nickname.value = preferences.nickname();

  function closeDevices() {
    options.hidden = true;
    microphone.setAttribute("aria-expanded", "false");
    microphone.removeAttribute("aria-activedescendant");
  }

  function show(next) {
    const changed = screen !== next;
    screen = next;
    app.dataset.screen = next;
    for (const id of ["entry", "waiting", "connected", "error"]) $(id).hidden = id !== next;
    title.hidden = !["entry", "waiting", "connected"].includes(next);
    closeDevices();
    if (changed) {
      const target = next === "error" ? $("error-message") : title.hidden ? $("brand") : title;
      target.focus();
    }
  }

  function setMeter(peak) {
    const value = micEnabled && Number.isFinite(peak) ? Math.round(Math.max(0, Math.min(1, peak)) * 100) : 0;
    meter.setAttribute("aria-valuenow", String(value));
    meter.setAttribute("aria-valuetext", `${value}%`);
    meter.firstElementChild.style.width = `${value}%`;
  }

  function clearConnection() {
    rows.clear();
    people.replaceChildren();
    micEnabled = false;
    micToggle.setAttribute("aria-pressed", "false");
    $("mic-text").textContent = "마이크 OFF";
    setMeter(0);
    prepared = false;
  }

  function fail(error) {
    clearConnection();
    // The coded failure comes first; the client's following 'error' state and rethrow must not replace its sentence.
    if (screen !== "error") $("error-message").textContent = errorText(error);
    show("error");
  }

  function updateEntry() {
    const valid = validNickname(nickname.value);
    nickname.setAttribute("aria-invalid", String(nickname.value.length > 0 && !valid));
    nickname.disabled = busy;
    microphone.disabled = busy;
    join.disabled = busy || !valid;
    entry.setAttribute("aria-busy", String(busy));
  }

  function updateDeviceHighlight() {
    [...options.children].forEach((option, index) => {
      option.classList.toggle("active", index === activeDevice);
      option.setAttribute("aria-selected", String(devices[index].deviceId === deviceId));
    });
    const active = options.children[activeDevice];
    if (active) {
      microphone.setAttribute("aria-activedescendant", active.id);
      active.scrollIntoView({ block: "nearest" });
    }
  }

  function renderDevices() {
    options.replaceChildren();
    devices.forEach((device, index) => {
      const option = doc.createElement("li");
      option.id = `microphone-option-${index}`;
      option.setAttribute("role", "option");
      option.textContent = device.label || "마이크";
      option.addEventListener("pointerdown", event => event.preventDefault());
      option.addEventListener("click", () => void chooseDevice(index));
      options.append(option);
    });
    const selected = devices.find(device => device.deviceId === deviceId);
    $("microphone-name").textContent = selected?.label || "마이크";
  }

  async function prepare(selectedId = deviceId) {
    const available = await client.prepareMicrophone(selectedId);
    devices = available;
    deviceId = selectedId ?? client.microphoneId ?? devices[0]?.deviceId ?? null;
    prepared = true;
    renderDevices();
  }

  async function openDevices() {
    if (busy) return;
    if (!options.hidden) { closeDevices(); return; }
    busy = true;
    updateEntry();
    try {
      if (!prepared) await prepare();
      if (screen !== "entry") return;
      options.hidden = false;
      microphone.setAttribute("aria-expanded", "true");
      // In a short window (a 1366x768 laptop at 125%) the list would open below the visible page.
      options.scrollIntoView({ block: "nearest" });
      activeDevice = Math.max(0, devices.findIndex(device => device.deviceId === deviceId));
      updateDeviceHighlight();
    } catch (error) { fail({ code: error.code }); }
    finally {
      busy = false;
      updateEntry();
      if (screen === "entry") microphone.focus();
    }
  }

  async function chooseDevice(index) {
    const selected = devices[index];
    closeDevices();
    if (!selected || selected.deviceId === deviceId) return;
    busy = true;
    updateEntry();
    try { await prepare(selected.deviceId); }
    catch (error) { fail({ code: error.code }); }
    finally {
      busy = false;
      updateEntry();
      if (screen === "entry") microphone.focus();
    }
  }

  function setRowVolume(row, value) {
    row.querySelector("input").value = String(value);
    row.querySelector("input").setAttribute("aria-valuetext", `${value}%`);
    row.querySelector(".vv").textContent = `${value}%`;
    row.querySelector(".slider").style.setProperty("--v", value / 100);
  }

  function onRoster({ members, selfId }) {
    if (["error", "ended"].includes(screen)) return;
    const others = members.filter(member => member.participantId !== selfId);
    const present = new Set(others.map(member => member.participantId));
    for (const [id, entry] of rows) {
      if (!present.has(id)) { entry.row.remove(); rows.delete(id); }
    }
    for (const member of others) {
      let entry = rows.get(member.participantId);
      if (!entry) {
        const row = $("person-template").content.firstElementChild.cloneNode(true);
        row.dataset.participantId = member.participantId;
        const input = row.querySelector("input");
        input.id = `volume-${member.participantId}`;
        row.querySelector("label").htmlFor = input.id;
        entry = { row, nick: member.nick };
        rows.set(member.participantId, entry);
        input.addEventListener("input", () => {
          const value = Number(input.value);
          client.setPeerVolume(member.participantId, value / 100);
          preferences.rememberVolume(entry.nick, value);
          setRowVolume(row, value);
        });
        // Apply memory before exposing the new person's controls.
        const value = preferences.volume(member.nick);
        client.setPeerVolume(member.participantId, value / 100);
        setRowVolume(row, value);
        people.append(row);
      } else if (entry.nick !== member.nick) {
        const value = preferences.volume(member.nick);
        client.setPeerVolume(member.participantId, value / 100);
        setRowVolume(entry.row, value);
      }
      entry.nick = member.nick;
      entry.row.querySelector("label").textContent = member.nick;
      // Connecting members have a row, but are not falsely labelled connected.
      entry.row.querySelector(".st").hidden = member.state !== "connected";
    }
    people.hidden = others.length === 0;
  }

  function onState({ state }) {
    if (localEnding) return;
    if (state === "pending" || state === "connecting") show("waiting");
    else if (state === "connected") show("connected");
    else if (["rejected", "ended", "error"].includes(state)) fail({ code: state });
  }

  async function begin() {
    show("loading");
    deviceId = null;
    devices = [];
    prepared = false;
    micEnabled = true;
    micToggle.setAttribute("aria-pressed", "true");
    $("mic-text").textContent = "마이크 ON";
    renderDevices();
    people.hidden = true;
    $("cancel").disabled = $("leave").disabled = false;
    try {
      invite = parseInvite(win.location.hash);
      client = createGuestClient({ onState, onRoster, onMeter: ({ peak }) => {
        if (screen === "connected") setMeter(peak);
      }, onError: fail });
      const room = await client.inspect(invite);
      title.textContent = `${room.hostNick} 님의 합방`;
      updateEntry();
      show("entry");
    } catch (error) { fail({ code: error.code }); }
  }

  // A reissued invite differs only in the fragment, so pasting it into this tab never reloads the page.
  async function reopen() {
    localEnding = true;
    closeDevices();
    try { await client?.dispose(); } catch { /* the dropped attempt has nothing left to show */ }
    clearConnection();
    localEnding = false;
    await begin();
  }

  async function finish(action) {
    localEnding = true;
    $("cancel").disabled = $("leave").disabled = true;
    closeDevices();
    try {
      await client[action]();
      await client.dispose();
      clearConnection();
      localEnding = false;
      if (action === "cancel") await begin();
      else show("ended");
    } catch (error) { fail({ code: error.code }); }
    finally { localEnding = false; }
  }

  nickname.addEventListener("input", () => {
    updateEntry();
    if (validNickname(nickname.value) || nickname.value === "") preferences.rememberNickname(nickname.value);
  });
  microphone.addEventListener("click", () => void openDevices());
  microphone.addEventListener("blur", closeDevices);
  microphone.addEventListener("keydown", event => {
    const keys = ["ArrowDown", "ArrowUp", "Home", "End", "Enter", " ", "Escape"];
    if (!keys.includes(event.key)) return;
    event.preventDefault();
    if (event.key === "Escape") { closeDevices(); return; }
    if (options.hidden) { void openDevices(); return; }
    if (event.key === "Enter" || event.key === " ") { void chooseDevice(activeDevice); return; }
    if (event.key === "Home") activeDevice = 0;
    else if (event.key === "End") activeDevice = devices.length - 1;
    else activeDevice = Math.max(0, Math.min(devices.length - 1, activeDevice + (event.key === "ArrowDown" ? 1 : -1)));
    updateDeviceHighlight();
  });
  entry.addEventListener("submit", async event => {
    event.preventDefault();
    if (busy || !validNickname(nickname.value)) return;
    nickname.value = cleanNickname(nickname.value);
    preferences.rememberNickname(nickname.value);
    busy = true;
    closeDevices();
    updateEntry();
    try {
      if (!prepared) await prepare();
      await client.requestJoin(invite, nickname.value);
    } catch (error) { fail({ code: error.code }); }
    finally { busy = false; updateEntry(); }
  });
  micToggle.addEventListener("click", () => {
    micEnabled = !micEnabled;
    client.setMicEnabled(micEnabled);
    micToggle.setAttribute("aria-pressed", String(micEnabled));
    $("mic-text").textContent = micEnabled ? "마이크 ON" : "마이크 OFF";
    if (!micEnabled) setMeter(0);
  });
  $("cancel").addEventListener("click", () => void finish("cancel"));
  $("leave").addEventListener("click", () => void finish("leave"));
  win.addEventListener("pagehide", () => { localEnding = true; void client?.dispose().catch(() => {}); });
  win.addEventListener("hashchange", () => void reopen());
  void begin();
}

if (typeof document !== "undefined") startApp();
