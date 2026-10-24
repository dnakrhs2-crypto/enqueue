"use strict";

(() => {
  const $ = id => document.getElementById(id);
  const root = $("remote");
  const wide = matchMedia("(min-width: 900px) and (orientation: landscape)");
  const listScroll = $("list-scroll");
  const rowViews = new Map();
  const cartViews = new Map();
  const activeViews = new Map();
  const scrollPositions = new Map();
  let lists = { gen: 0, active: null, mode: "show", tabs: [], cues: {}, playhead: {} };
  let state = { playing: [], waits: [], counts: {}, next: null, goLocked: true };
  let playingByCue = new Map();
  let waitsByCue = new Map();
  let stateAt = 0;
  let mode = "show";
  let renderedTab = null;
  let localActive = false;
  let socket = null;
  let requestId = 0;
  let touchId = 0;
  let heldGo = null;
  let gotHello = false;
  let gotState = false;
  let terminal = false;
  let suspended = document.hidden;
  let probe = null;
  let lastMessage = 0;
  let retryStep = 0;
  let retryTimer = 0;
  let toastTimer = 0;
  let locked = true;
  let drag = null;
  let pendingMove = null;
  let observedHead = null;
  let observedHeadTab = null;
  let listTouching = false;
  let fingerScrollUntil = 0;
  let frameId = 0;
  let previousFrame = 0;
  let clockSamples = [];
  let clockOffset = null;

  function element(tag, className, text) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined) node.textContent = text;
    return node;
  }

  function icon(name) {
    const node = element("span", `icon icon-${name}`);
    node.setAttribute("aria-hidden", "true");
    return node;
  }

  function button(className, label, iconName) {
    const node = element("button", className);
    node.type = "button";
    node.disabled = locked;
    node.setAttribute("aria-label", label);
    if (iconName) node.append(icon(iconName));
    return node;
  }

  function setText(node, text) {
    const value = String(text ?? "");
    if (node.textContent !== value) node.textContent = value;
  }

  function ready() {
    return socket?.readyState === WebSocket.OPEN && gotHello && gotState && !probe && !suspended && !terminal;
  }

  function updateClockOffset(at) {
    clockSamples = clockSamples.filter(sample => at - sample.at <= 20000);
    if (clockSamples.length) clockOffset = Math.max(...clockSamples.map(sample => sample.offset));
    // Keep the last estimate if no sample remains in the window.
  }

  function observeClock(serverNow, at) {
    if (!Number.isFinite(serverNow)) return;
    clockSamples.push({ at, offset: serverNow - at });
    updateClockOffset(at);
  }

  function needsTimestamp(t, data) {
    return t === "go" ? data.down === true
      : t === "cmd" ? data.cmd === "pause" || data.cmd === "fade"
      : ["cue", "wait", "fire", "stopcue", "tab", "playhead", "move", "mode"].includes(t);
  }

  // Handshake, ping and GO release may pass the UI lock. No requests are queued or retried.
  function send(t, data = {}, throughLock = false) {
    const at = performance.now(); // Called synchronously by the gesture that commits this operation.
    if (socket?.readyState !== WebSocket.OPEN || terminal || (!throughLock && !ready())) return null;
    const id = ++requestId;
    const message = { t, id, ...data };
    if (needsTimestamp(t, data)) {
      updateClockOffset(at);
      if (clockOffset !== null) message.st = Math.round(at + clockOffset);
    }
    socket.send(JSON.stringify(message));
    return id;
  }

  function syncLocks() {
    locked = !ready();
    root.classList.toggle("is-locked", locked);
    root.classList.toggle("is-connected", !locked);
    setText($("connection-label"), terminal ? "연결 끊김" : locked ? "다시 연결 중" : "연결됨");
    $("connection-notice").hidden = !locked;
    setText($("connection-notice"), terminal ? "QR을 다시 찍어 주세요" : "다시 연결 중");
    root.querySelectorAll("button").forEach(node => { node.disabled = locked; });
    $("go").disabled = locked || Boolean(state.goLocked);
    rowViews.forEach(view => {
      view.node.setAttribute("aria-disabled", String(locked));
      view.node.tabIndex = locked ? -1 : 0;
      view.handle.disabled = locked || mode !== "edit" || Boolean(pendingMove);
    });
    syncAnimation();
  }

  function showError(message) {
    if (!message) return;
    setText($("toast"), message);
    $("toast").hidden = false;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => { $("toast").hidden = true; }, 2600);
  }

  function deviceName() {
    const ua = navigator.userAgent;
    const platform = /iPad/.test(ua) || (/Macintosh/.test(ua) && navigator.maxTouchPoints > 1) ? "iPad"
      : /iPhone|iPod/.test(ua) ? "iPhone" : /Android/.test(ua) ? "Android"
      : /Windows/.test(ua) ? "Windows" : "Mac";
    const browser = /Edg(?:e|A|iOS)?\//.test(ua) ? "Edge"
      : /Firefox|FxiOS/.test(ua) ? "Firefox" : /SamsungBrowser/.test(ua) ? "Samsung Internet"
      : /Chrome|CriOS/.test(ua) ? "크롬" : "사파리";
    return `${platform} · ${browser}`;
  }

  function releaseGo() {
    if (!heldGo) return;
    const held = heldGo;
    heldGo = null;
    $("go").classList.remove("is-pressed");
    send("go", { down: false, touch: held.touch }, true);
    if (held.pointer !== undefined && $("go").hasPointerCapture(held.pointer)) $("go").releasePointerCapture(held.pointer);
  }

  function clearPendingMove() {
    if (!pendingMove) return;
    rowViews.get(pendingMove.cue)?.node.classList.remove("pending-move");
    pendingMove = null;
  }

  function disconnected(current) {
    if (socket !== current) return;
    releaseGo();
    cancelDrag();
    clearPendingMove();
    socket = null;
    gotHello = false;
    gotState = false;
    probe = null;
    syncLocks();
    if (current.readyState < WebSocket.CLOSING) current.close();
    if (!terminal && !retryTimer) {
      const delay = [500, 1000, 2000, 3000][Math.min(retryStep++, 3)];
      retryTimer = setTimeout(() => { retryTimer = 0; connect(); }, delay);
    }
  }

  function connect() {
    if (terminal || socket) return;
    requestId = 0;
    gotHello = false;
    gotState = false;
    clockSamples = [];
    clockOffset = null;
    lastMessage = performance.now();
    const key = new URLSearchParams(location.hash.slice(1)).get("k") || "";
    const current = new WebSocket(`ws://${location.host}/ws?k=${encodeURIComponent(key)}`);
    socket = current;
    syncLocks();
    current.addEventListener("message", event => {
      if (socket !== current) return;
      lastMessage = performance.now();
      let message;
      try { message = JSON.parse(event.data); } catch { return; }
      switch (message.t) {
        case "hello":
          observeClock(message.now, lastMessage);
          gotHello = true;
          setText($("pc-name"), message.pc);
          setText($("project-name"), message.project ? ` · ${message.project}` : "");
          $("project-identity").title = `${message.pc}${message.project ? ` · ${message.project}` : ""}`;
          send("hi", { device: deviceName() }, true);
          break;
        case "lists":
          receiveLists(message);
          break;
        case "state":
          observeClock(message.now, lastMessage);
          state = message;
          stateAt = performance.now();
          gotState = true;
          retryStep = 0;
          if (probe?.acked) probe = null; // Only a state after the ping acknowledgement unlocks a resumed page.
          applyMode(message.mode);
          indexState();
          renderState();
          syncLocks();
          break;
        case "ack":
          if (probe && message.id === probe.id && message.ok) probe.acked = true;
          if (!message.ok) {
            if (pendingMove?.id === message.id) { clearPendingMove(); syncLocks(); }
            showError(message.msg);
          }
          break;
        case "bye":
          if (message.reason === "key" || message.reason === "newkey") terminal = true;
          disconnected(current);
          break;
      }
    });
    current.addEventListener("close", () => disconnected(current));
    current.addEventListener("error", () => disconnected(current));
  }

  function verifyConnection() {
    suspended = document.hidden;
    if (suspended || terminal) return;
    if (socket && performance.now() - lastMessage >= 3000) {
      disconnected(socket);
      return;
    }
    if (socket?.readyState === WebSocket.OPEN && gotHello && !probe) {
      releaseGo();
      cancelDrag();
      probe = { id: send("ping", {}, true), acked: false };
    }
    syncLocks();
  }

  document.addEventListener("visibilitychange", () => {
    if (document.hidden) {
      releaseGo();
      cancelDrag();
      suspended = true;
      syncLocks();
    } else verifyConnection();
  });
  window.addEventListener("pageshow", verifyConnection);
  window.addEventListener("pagehide", () => { releaseGo(); cancelDrag(); suspended = true; syncLocks(); });
  window.addEventListener("blur", releaseGo);
  setInterval(() => {
    if (socket && performance.now() - lastMessage >= 3000) disconnected(socket);
  }, 250);

  $("go").addEventListener("pointerdown", event => {
    if (!event.isPrimary || event.button !== 0 || heldGo || !ready() || state.goLocked) return;
    event.preventDefault();
    const touch = ++touchId;
    if (send("go", { down: true, touch }) === null) return;
    heldGo = { pointer: event.pointerId, touch };
    $("go").classList.add("is-pressed");
    $("go").setPointerCapture(event.pointerId);
  });
  ["pointerup", "pointercancel", "lostpointercapture"].forEach(type => {
    $("go").addEventListener(type, event => { if (heldGo?.pointer === event.pointerId) releaseGo(); });
  });
  // Keyboard activation has its own press/release pair; the synthetic click is deliberately unused.
  $("go").addEventListener("keydown", event => {
    if (![" ", "Enter"].includes(event.key)) return;
    event.preventDefault();
    if (event.repeat || heldGo || !ready() || state.goLocked) return;
    const touch = ++touchId;
    if (send("go", { down: true, touch }) !== null) {
      heldGo = { key: event.key, touch };
      $("go").classList.add("is-pressed");
    }
  });
  $("go").addEventListener("keyup", event => { if (heldGo?.key === event.key) releaseGo(); });
  $("go").addEventListener("blur", releaseGo);
  $("panic").addEventListener("pointerdown", event => {
    if (!event.isPrimary || event.button !== 0) return;
    event.preventDefault();
    send("cmd", { cmd: "panic" });
  });
  $("panic").addEventListener("keydown", event => {
    if ([" ", "Enter"].includes(event.key)) {
      event.preventDefault();
      if (!event.repeat) send("cmd", { cmd: "panic" });
    }
  });
  document.querySelectorAll("[data-command]").forEach(node => node.addEventListener("click", () => send("cmd", { cmd: node.dataset.command })));
  document.querySelectorAll("[data-show]").forEach(node => node.addEventListener("click", () => send("mode", { show: node.dataset.show === "true" })));

  // Native scrolling remains enabled. Crossing 10px permanently cancels that tap, even if it returns.
  function bindTap(node, action, maxTime = Infinity) {
    let tap = null;
    node.addEventListener("pointerdown", event => {
      if (!ready() || !event.isPrimary || event.button !== 0 || event.target.closest(".drag-handle, .cart-stop")) return;
      tap = { pointer: event.pointerId, x: event.clientX, y: event.clientY, at: performance.now(), moved: false };
    });
    node.addEventListener("pointermove", event => {
      if (tap?.pointer === event.pointerId && Math.hypot(event.clientX - tap.x, event.clientY - tap.y) > 10) tap.moved = true;
    });
    node.addEventListener("pointerup", event => {
      if (!tap || tap.pointer !== event.pointerId) return;
      const press = tap;
      tap = null;
      if (!press.moved && Math.hypot(event.clientX - press.x, event.clientY - press.y) <= 10 && performance.now() - press.at <= maxTime && ready()) action();
    });
    ["pointercancel", "pointerleave"].forEach(type => node.addEventListener(type, () => { tap = null; }));
    node.addEventListener("click", event => {
      if (event.detail === 0 && ready() && !event.target.closest(".drag-handle, .cart-stop")) action();
    });
    if (node.tagName !== "BUTTON") node.addEventListener("keydown", event => {
      if ([" ", "Enter"].includes(event.key) && !event.target.closest(".drag-handle")) {
        event.preventDefault();
        if (!event.repeat && ready()) action();
      }
    });
  }

  function applyMode(nextMode) {
    if (!nextMode) return;
    mode = nextMode;
    if (mode !== "edit") cancelDrag();
    root.classList.toggle("mode-edit", mode === "edit");
    document.querySelectorAll("[data-show]").forEach(node => node.setAttribute("aria-pressed", String((node.dataset.show === "true") === (mode === "show"))));
  }

  function moveReflected(pending) {
    const cues = lists.cues[pending.tab] || [];
    const index = cues.findIndex(cue => cue.id === pending.cue);
    if (index < 0 || (cues[index].parent ?? null) !== pending.parent) return false;
    let after = index + 1;
    while (after < cues.length && cues[after].depth > cues[index].depth) after++;
    if (pending.before !== null) return cues[after]?.id === pending.before;
    // At the parent's end, the next visible row is outside that parent's children.
    return after === cues.length || cues[after].depth < cues[index].depth;
  }

  function receiveLists(message) {
    const skipFollow = Boolean(drag) || listTouching || performance.now() < fingerScrollUntil;
    cancelDrag();
    if (renderedTab) scrollPositions.set(renderedTab, { list: listScroll.scrollTop, cart: $("cart-scroll").scrollTop });
    lists = message;
    applyMode(message.mode);
    if (pendingMove && lists.gen > pendingMove.gen && moveReflected(pendingMove)) clearPendingMove();
    renderTabs();
    renderCues();
    renderState();
    updateLayout();
    followPlayhead(skipFollow);
    syncLocks();
  }

  function followPlayhead(skip) {
    const head = lists.playhead[lists.active] ?? null;
    const changed = head !== observedHead || lists.active !== observedHeadTab;
    observedHead = head;
    observedHeadTab = lists.active;
    if (changed && head && !skip && !$("content").hidden && !listScroll.hidden) {
      rowViews.get(head)?.node.scrollIntoView({ block: "nearest", inline: "nearest" });
    }
  }

  // touch events survive the pointercancel issued when the browser takes over a scroll.
  listScroll.addEventListener("touchstart", event => {
    if (event.target.closest(".drag-handle")) return;
    listTouching = true;
    fingerScrollUntil = performance.now() + 150;
  }, { passive: true });
  ["touchend", "touchcancel"].forEach(type => listScroll.addEventListener(type, event => {
    listTouching = event.touches.length > 0;
  }, { passive: true }));
  listScroll.addEventListener("scroll", () => {
    if (listTouching || performance.now() < fingerScrollUntil) fingerScrollUntil = performance.now() + 150;
  }, { passive: true });

  function renderTabs() {
    const nodes = lists.tabs.map(tab => {
      const node = button("tab", tab.name, tab.kind === "cart" ? "cart" : "list");
      node.dataset.tab = tab.id;
      node.title = tab.name;
      node.setAttribute("role", "tab");
      node.append(element("span", "", tab.name));
      node.addEventListener("click", () => {
        if (!ready()) return;
        localActive = false;
        send("tab", { tab: tab.id });
        updateLayout();
      });
      return node;
    });
    const active = button("tab", "활성 큐");
    active.dataset.local = "active";
    active.textContent = "활성 큐";
    active.setAttribute("role", "tab");
    active.addEventListener("click", () => {
      if (!ready()) return;
      localActive = true;
      cancelDrag();
      updateLayout();
    });
    $("tabs").replaceChildren(...nodes, active);
  }

  function updateLayout() {
    const showActive = localActive && !wide.matches;
    $("content").hidden = showActive;
    $("active-panel").hidden = !wide.matches && !showActive;
    $("tabs").querySelectorAll(".tab").forEach(node => {
      const selected = node.dataset.local === "active" ? showActive : !showActive && node.dataset.tab === lists.active;
      node.setAttribute("aria-selected", String(selected));
    });
    revealSelectedTab();
  }
  function updateTabEdges() {
    const tabs = $("tabs");
    $("tabs-strip").classList.toggle("can-scroll-left", tabs.scrollLeft > 1);
    $("tabs-strip").classList.toggle("can-scroll-right", tabs.scrollLeft + tabs.clientWidth < tabs.scrollWidth - 1);
  }
  function revealSelectedTab() {
    $("tabs").querySelector('[aria-selected="true"]')?.scrollIntoView({ block: "nearest", inline: "nearest" });
    updateTabEdges();
  }
  $("tabs").addEventListener("scroll", updateTabEdges, { passive: true });

  function fitNextName() {
    const name = $("next-name");
    name.parentElement.classList.remove("is-compact");
    if (name.scrollHeight > name.clientHeight + 1 || name.scrollWidth > name.clientWidth + 1) {
      name.parentElement.classList.add("is-compact");
    }
  }
  const sizeObserver = new ResizeObserver(entries => {
    if (entries.some(entry => entry.target === $("tabs-strip"))) revealSelectedTab();
    if (entries.some(entry => entry.target === $("next-name").closest(".next-cue"))) fitNextName();
  });
  sizeObserver.observe($("tabs-strip"));
  sizeObserver.observe($("next-name").closest(".next-cue"));
  wide.addEventListener("change", () => { cancelDrag(); updateLayout(); });
  window.addEventListener("resize", cancelDrag);

  function renderCues() {
    const tab = lists.tabs.find(item => item.id === lists.active);
    renderedTab = tab?.id ?? null;
    rowViews.clear();
    cartViews.clear();
    $("cue-list").replaceChildren();
    $("cart-grid").replaceChildren();
    listScroll.hidden = tab?.kind !== "list";
    $("cart-scroll").hidden = tab?.kind !== "cart";
    if (!tab) return;
    const cues = lists.cues[tab.id] || [];
    if (tab.kind === "list") {
      $("cue-list").style.setProperty("--no-ch", Math.max(1, ...cues.map(cue => String(cue.no).length)));
      const fragment = document.createDocumentFragment();
      cues.forEach(cue => {
        const node = element("div", "cue-row");
        node.dataset.cue = cue.id;
        node.setAttribute("role", "button");
        node.setAttribute("aria-label", `${cue.no} ${cue.name}`);
        node.style.setProperty("--depth", cue.depth || 0);
        const number = element("span", "cue-number", cue.no);
        const name = element("span", "cue-name");
        if (cue.kind === "group") name.append(icon("folder"));
        const title = element("span", "cue-name-text", cue.name);
        title.title = cue.name;
        const wait = element("span", "cue-wait");
        const waitName = element("span");
        const waitTime = element("span", "mono");
        wait.append(waitName, waitTime);
        name.append(title, wait);
        const time = element("span", "cue-time", cue.len);
        const handle = button("drag-handle", `${cue.no} ${cue.name}`, "drag");
        handle.addEventListener("pointerdown", event => beginDrag(event, cue, tab.id, node, handle));
        node.append(number, name, time, handle);
        bindTap(node, () => send("playhead", { tab: tab.id, cue: cue.id }), 500);
        node.classList.toggle("pending-move", pendingMove?.cue === cue.id);
        rowViews.set(cue.id, { node, time, waitName, waitTime, handle, cue });
        fragment.append(node);
      });
      $("cue-list").append(fragment);
      listScroll.scrollTop = scrollPositions.get(tab.id)?.list || 0;
    } else {
      $("cart-grid").style.setProperty("--cols", tab.cols);
      $("cart-grid").style.setProperty("--rows", tab.rows);
      const slots = new Map(cues.map(cue => [cue.slot, cue])); // PC CueCartView slots are zero based.
      const fragment = document.createDocumentFragment();
      for (let slot = 0; slot < tab.cols * tab.rows; slot++) {
        const cue = slots.get(slot);
        const node = element("div", "cart-cell");
        node.dataset.slot = slot;
        if (!cue) {
          node.classList.add("is-empty");
        } else {
          node.dataset.cue = cue.id;
          const fire = button("cart-fire", `${cue.no} ${cue.name}`);
          const time = element("span", "cart-time mono", cue.len);
          fire.append(element("span", "cart-number mono", cue.no), element("span", "cart-name", cue.name), time);
          bindTap(fire, () => send("fire", { cue: cue.id }));
          const stop = button("cart-stop", "×", "close");
          stop.hidden = true;
          stop.addEventListener("click", () => send("stopcue", { cue: cue.id }));
          node.append(fire, stop);
          cartViews.set(cue.id, { node, time, stop, cue });
        }
        fragment.append(node);
      }
      $("cart-grid").append(fragment);
      $("cart-scroll").scrollTop = scrollPositions.get(tab.id)?.cart || 0;
    }
  }

  function indexState() {
    playingByCue = new Map();
    for (const item of state.playing) {
      const existing = playingByCue.get(item.id);
      if (!existing || item.order > existing.order) playingByCue.set(item.id, item);
    }
    waitsByCue = new Map(state.waits.map(item => [item.cue, item]));
  }

  function setStatus(node, item, wait = null) {
    ["play", "pause", "fade"].forEach(status => node.classList.toggle(`is-${status}`, item?.st === status));
    node.classList.toggle("is-wait", !item && Boolean(wait));
  }

  function waitLabel(kind) { return kind === "pre" ? "프리웨이트" : kind === "post" ? "포스트웨이트" : "대기"; }

  function groupStatuses() {
    const statuses = new Map();
    const rank = { wait: 1, pause: 2, play: 3 };
    const merge = (id, status) => {
      if (status && (!statuses.has(id) || rank[status] > rank[statuses.get(id)])) statuses.set(id, status);
    };
    const cues = lists.cues[lists.active] || [];
    // Children precede their parent in this reverse walk, including nested groups.
    for (let i = cues.length - 1; i >= 0; i--) {
      const cue = cues[i];
      const item = playingByCue.get(cue.id);
      merge(cue.id, item ? item.st === "pause" ? "pause" : "play" : waitsByCue.has(cue.id) ? "wait" : null);
      if (cue.parent) merge(cue.parent, statuses.get(cue.id));
    }
    return statuses;
  }

  function renderState() {
    const nextChanged = $("next-name").textContent !== String(state.next?.name ?? "")
      || $("next-number").textContent !== String(state.next?.no ?? "");
    setText($("next-number"), state.next?.no);
    setText($("next-name"), state.next?.name);
    $("next-name").title = state.next?.name || "";
    setText($("next-length"), state.next?.len);
    $("next-length-line").hidden = !state.next;
    if (nextChanged) fitNextName();
    const groups = groupStatuses();
    rowViews.forEach(view => {
      if (view.cue.kind === "group") {
        const status = groups.get(view.cue.id);
        setStatus(view.node, status && status !== "wait" ? { st: status } : null, status === "wait");
      } else setStatus(view.node, playingByCue.get(view.cue.id), waitsByCue.get(view.cue.id));
      view.node.classList.toggle("is-next", lists.playhead[lists.active] === view.cue.id);
    });
    cartViews.forEach(view => {
      const item = playingByCue.get(view.cue.id);
      setStatus(view.node, item);
      view.stop.hidden = !item;
    });
    setText($("playing-count"), state.counts.playing || 0);
    setText($("waiting-count"), state.counts.waiting || 0);
    const items = [
      ...[...state.playing].sort((a, b) => a.order - b.order).map(item => ({ key: `p:${item.id}:${item.order}`, item, wait: false })),
      ...state.waits.map(item => ({ key: `w:${item.cue}:${item.kind}:${item.startId}`, item, wait: true }))
    ];
    const keys = new Set(items.map(entry => entry.key));
    activeViews.forEach((view, key) => { if (!keys.has(key)) { view.node.remove(); activeViews.delete(key); } });
    items.forEach((entry, index) => {
      let view = activeViews.get(entry.key);
      if (!view) {
        const node = element("article", "active-card");
        const info = element("div", "active-info");
        const title = element("div", "active-title");
        const number = element("span", "active-number mono");
        const name = element("span");
        title.append(number, name);
        const timeLine = element("div");
        const kind = element("span", "active-kind");
        const time = element("span", "active-time mono");
        timeLine.append(kind, time);
        const progress = element("div", "progress");
        const fill = element("span", "progress-fill");
        progress.append(fill);
        info.append(title, timeLine, progress);
        const actions = element("div", "active-actions");
        const pause = button("round-button", "일시정지", "pause");
        const stop = button("round-button stop", "×", "close");
        actions.append(pause, stop);
        node.append(info, actions);
        view = { node, number, name, title, time, kind, fill, pause, stop, entry };
        pause.addEventListener("click", () => {
          const item = view.entry.item;
          send("cue", { op: item.st === "pause" ? "resume" : "pause", cue: item.id, order: item.order });
        });
        stop.addEventListener("click", () => {
          const { item, wait } = view.entry;
          if (wait) send("wait", { cue: item.cue, kind: item.kind, startId: item.startId });
          else send("cue", { op: "stop", cue: item.id, order: item.order });
        });
        activeViews.set(entry.key, view);
      }
      view.entry = entry;
      const { item, wait } = entry;
      setStatus(view.node, wait ? null : item, wait ? item : null);
      view.node.dataset.cue = wait ? item.cue : item.id;
      setText(view.number, item.no);
      setText(view.name, item.name);
      view.title.title = `${item.no} ${item.name}`;
      setText(view.kind, wait ? waitLabel(item.kind) : "");
      view.pause.hidden = wait;
      view.pause.classList.toggle("resume", item.st === "pause");
      view.pause.setAttribute("aria-label", item.st === "pause" ? "재개" : "일시정지");
      view.pause.firstElementChild.className = `icon icon-${item.st === "pause" ? "play" : "pause"}`;
      const atIndex = $("active-cues").children[index];
      if (atIndex !== view.node) $("active-cues").insertBefore(view.node, atIndex || null);
    });
    updateTimes(performance.now());
  }

  function timeText(seconds, remaining = false) {
    if (seconds === null || seconds === undefined || !Number.isFinite(seconds) || seconds < 0) return "∞";
    const tenths = Math.max(0, Math.ceil(seconds * 10 - 0.0001));
    const minutes = Math.floor(tenths / 600);
    const secondsPart = String(Math.floor(tenths / 10) % 60).padStart(2, "0");
    return `${remaining ? "-" : ""}${minutes}:${secondsPart}.${tenths % 10}`;
  }

  function interpolated(item, now) {
    if (item.remain === null || item.remain < 0) return null;
    const delta = !locked && (item.st === "play" || item.st === "fade") ? (now - stateAt) / 1000 : 0;
    return Math.max(0, item.remain - delta);
  }

  function updateTimes(now) {
    rowViews.forEach(view => {
      const item = playingByCue.get(view.cue.id);
      const wait = waitsByCue.get(view.cue.id);
      setText(view.time, item ? timeText(interpolated(item, now), true) : view.cue.len);
      setText(view.waitName, wait ? `${waitLabel(wait.kind)} ` : "");
      setText(view.waitTime, wait ? timeText(wait.remain) : "");
    });
    cartViews.forEach(view => {
      const item = playingByCue.get(view.cue.id);
      setText(view.time, item ? timeText(interpolated(item, now), true) : view.cue.len);
    });
    activeViews.forEach(view => {
      const { item, wait } = view.entry;
      setText(view.time, wait ? timeText(item.remain) : timeText(interpolated(item, now), true));
      const delta = !locked && !wait && item.st !== "pause" ? (now - stateAt) / 1000 : 0;
      const progress = wait ? 0 : (item.progress || 0) + (item.dur > 0 ? delta / item.dur : 0);
      view.fill.style.transform = `scaleX(${Math.max(0, Math.min(1, progress))})`;
    });
  }

  function beginDrag(event, cue, tab, node, handle) {
    event.stopPropagation();
    if (!ready() || mode !== "edit" || pendingMove || drag || !event.isPrimary || event.button !== 0) return;
    event.preventDefault();
    const rect = node.getBoundingClientRect();
    const ghost = node.cloneNode(true);
    ghost.classList.add("drag-ghost");
    ghost.classList.remove("pending-move", "drag-source");
    ghost.setAttribute("aria-hidden", "true");
    ghost.inert = true;
    ghost.style.left = `${rect.left}px`;
    ghost.style.top = `${rect.top}px`;
    ghost.style.width = `${rect.width}px`;
    ghost.style.setProperty("--no-ch", $("cue-list").style.getPropertyValue("--no-ch"));
    root.append(ghost);
    node.classList.add("drag-source");
    const cues = lists.cues[tab];
    const descendants = new Set([cue.id]);
    // Flattened lists preserve parent-before-child order, including nested groups.
    cues.forEach(item => { if (descendants.has(item.parent)) descendants.add(item.id); });
    drag = { pointer: event.pointerId, cue, tab, node, handle, ghost, descendants,
      x: event.clientX, y: event.clientY, startX: event.clientX, startY: event.clientY, moved: false, target: null };
    handle.setPointerCapture(event.pointerId);
    calculateDrop();
    syncAnimation();
  }

  function setDropTarget(target, groupNode = null, offset = 0) {
    drag.target = target;
    if (drag.groupNode !== groupNode) {
      drag.groupNode?.classList.remove("drop-group");
      drag.groupNode = groupNode;
      groupNode?.classList.add("drop-group");
    }
    $("drop-line").hidden = !target || Boolean(groupNode);
    if (target && !groupNode) $("drop-line").style.top = `${Math.max(0, offset - 1)}px`;
  }

  function calculateDrop() {
    if (!drag) return;
    const bounds = listScroll.getBoundingClientRect();
    if (drag.x < bounds.left || drag.x > bounds.right || drag.y < bounds.top - 24 || drag.y > bounds.bottom + 24) {
      setDropTarget(null);
      return;
    }
    const cues = lists.cues[drag.tab];
    let below = null;
    for (const cue of cues) {
      const node = rowViews.get(cue.id).node;
      const rect = node.getBoundingClientRect();
      // PC CueTable::groupAtY: the middle half appends to the group, edges insert between rows.
      if (cue.kind === "group" && drag.y >= rect.top + rect.height / 4 && drag.y < rect.bottom - rect.height / 4) {
        if (drag.descendants.has(cue.id)) setDropTarget(null);
        else setDropTarget({ before: null, parent: cue.id }, node);
        return;
      }
      if (drag.y < rect.top + rect.height / 2) { below = cue; break; }
    }
    if (below && (drag.descendants.has(below.id) || drag.descendants.has(below.parent))) {
      setDropTarget(null);
      return;
    }
    const offset = below ? rowViews.get(below.id).node.offsetTop : (cues.length ? rowViews.get(cues[cues.length - 1].id).node.offsetTop + 52 : 0);
    setDropTarget({ before: below?.id ?? null, parent: below?.parent ?? null }, null, offset);
  }

  function cancelDrag() {
    if (!drag) return;
    setDropTarget(null);
    const previous = drag;
    drag = null;
    previous.ghost.remove();
    previous.node.classList.remove("drag-source");
    if (previous.handle.hasPointerCapture(previous.pointer)) previous.handle.releasePointerCapture(previous.pointer);
    syncAnimation();
  }

  window.addEventListener("pointermove", event => {
    if (drag?.pointer !== event.pointerId) return;
    drag.x = event.clientX;
    drag.y = event.clientY;
    if (Math.hypot(drag.x - drag.startX, drag.y - drag.startY) > 4) drag.moved = true;
    drag.ghost.style.transform = `translateY(${drag.y - drag.startY}px)`;
    calculateDrop();
  });
  window.addEventListener("pointerup", event => {
    if (drag?.pointer !== event.pointerId) return;
    calculateDrop();
    const previous = drag;
    const target = drag.target;
    cancelDrag();
    if (!target || !previous.moved || !ready() || mode !== "edit") return;
    const payload = { tab: previous.tab, cue: previous.cue.id, ...target };
    const id = send("move", payload);
    if (id !== null) {
      pendingMove = { id, gen: lists.gen, ...payload };
      previous.node.classList.add("pending-move");
      syncLocks();
    }
  });
  window.addEventListener("pointercancel", event => { if (drag?.pointer === event.pointerId) cancelDrag(); });
  root.addEventListener("lostpointercapture", event => { if (drag?.pointer === event.pointerId) cancelDrag(); });

  function needsAnimation() {
    return !locked && (Boolean(drag) || state.waits.length > 0 || state.playing.some(item => item.st !== "pause"));
  }
  function syncAnimation() {
    if (needsAnimation()) {
      if (!frameId) {
        previousFrame = performance.now();
        frameId = requestAnimationFrame(frame);
      }
    } else if (frameId) {
      cancelAnimationFrame(frameId);
      frameId = 0;
    }
  }
  function frame(now) {
    frameId = 0;
    if (!needsAnimation()) return;
    updateTimes(now);
    if (drag) {
      const rect = listScroll.getBoundingClientRect();
      const edge = Math.min(48, rect.height / 3);
      const direction = drag.y < rect.top + edge ? -Math.min(1, (rect.top + edge - drag.y) / edge)
        : drag.y > rect.bottom - edge ? Math.min(1, (drag.y - rect.bottom + edge) / edge) : 0;
      if (direction && drag.x >= rect.left && drag.x <= rect.right) {
        listScroll.scrollTop += direction * Math.min(now - previousFrame, 32) * .6;
        calculateDrop();
      }
    }
    previousFrame = now;
    frameId = requestAnimationFrame(frame);
  }
  updateLayout();
  connect();
})();
