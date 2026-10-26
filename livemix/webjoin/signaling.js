export const API_BASE = "https://xn--2j1bo5s81gjmag88b.com/api/livemix/collab/";
export const STUN = Object.freeze([{ urls: "stun:stun.l.google.com:19302" }, { urls: "stun:stun.cloudflare.com:3478" }]);
export class SignalError extends Error {
  constructor(code, status = 0) { super(code); this.code = code; this.status = status; }
}
export function cursorValue(value) {
  if (typeof value !== "string" || !/^(0|[1-9][0-9]{0,19})$/.test(value) || BigInt(value) > 0xffffffffffffffffn)
    throw new SignalError("bad_response");
  return BigInt(value);
}
export class Signaling {
  constructor({ apiBase = API_BASE, fetchImpl = globalThis.fetch, timeoutMs = 30000, onEvent = () => {}, onError = () => {} } = {}) {
    // Browsers throw "Illegal invocation" when the global fetch runs with this = Signaling, so never call it as a method.
    this.base = apiBase; this.fetch = (url, init) => fetchImpl(url, init); this.timeout = timeoutMs;
    this.onEvent = onEvent; this.onError = onError;
    this.pending = new Set(); this.stopped = false; this.polling = false;
    this.code = null; this.auth = null; this.cursor = "0"; this.generation = 0;
    this.nextSignal = 1; this.signalTail = Promise.resolve();
  }
  async post(path, body, retry = true) {
    const json = JSON.stringify({ ...body, apiVersion: 2 });
    for (let attempt = 0; ; attempt++) {
      if (this.stopped) throw new SignalError("cancelled");
      const controller = new AbortController(); this.pending.add(controller);
      const timer = setTimeout(() => controller.abort(), this.timeout);
      let problem;
      try {
        const response = await this.fetch(new URL(path, this.base), { method: "POST", credentials: "omit",
          headers: { "Content-Type": "text/plain" }, body: json, signal: controller.signal });
        const result = await response.json();
        if (!response.ok) throw new SignalError(typeof result.error === "string" ? result.error : "http", response.status);
        if (result.apiVersion !== 2) throw new SignalError("bad_response");
        return result;
      } catch (error) {
        problem = error instanceof SignalError ? error : new SignalError(controller.signal.aborted ? "timeout" : "network");
      } finally { clearTimeout(timer); this.pending.delete(controller); }
      if (this.stopped) throw new SignalError("cancelled");
      if (!retry || attempt >= 2 || (problem.status && problem.status < 500) || problem.code === "bad_response") throw problem;
      await new Promise(resolve => setTimeout(resolve, 250 * (attempt + 1)));
    }
  }
  inspect(invite) { return this.post("web/request", { ...invite, action: "inspect" }); }
  async join(invite, nick, connectionId, capabilities) {
    const result = await this.post("web/request", { ...invite, action: "join", nick, connectionId, protocols: [2], capabilities });
    if (!/^[0-9a-f]{64}$/.test(result.requestToken) || result.state !== "pending") throw new SignalError("bad_response");
    cursorValue(result.cursor);
    this.code = invite.code; this.auth = { requestToken: result.requestToken };
    this.cursor = result.cursor;
    return result;
  }
  startPolling() {
    if (this.polling || !this.auth || this.stopped) return;
    this.polling = true;
    this.pollTask = this.poll().catch(error => { if (!this.stopped) this.onError(error.code ?? "network"); })
      .finally(() => { this.polling = false; });
  }
  async poll() {
    while (!this.stopped && this.auth) {
      let result;
      try { result = await this.post("web/events", { code: this.code, ...this.auth, cursor: this.cursor }); }
      catch (error) {
        if (!this.stopped && (error.code === "network" || error.code === "timeout" || error.status >= 500)) continue;
        throw error;
      }
      if (this.stopped) return;
      const responseCursor = cursorValue(result.cursor);
      if (!Array.isArray(result.events) || result.events.length > 32 || responseCursor < cursorValue(this.cursor))
        throw new SignalError("bad_response");
      let switched = false;
      for (const event of result.events) {
        const id = cursorValue(event.id);
        if (id <= cursorValue(this.cursor)) continue;
        if (id > responseCursor) throw new SignalError("bad_response");
        if (event.type === "accepted" && this.auth.requestToken) {
          const d = event.data;
          if (!/^[0-9a-f]{64}$/.test(d.connectionToken) || !Number.isInteger(d.connectionGeneration) ||
              d.connectionGeneration < 1 || d.connectionGeneration > 0xffffffff) throw new SignalError("bad_response");
          // The request response cursor may have scanned hidden connection events.
          // Switch at accepted.id so an offer already in the room log is not lost.
          await this.onEvent(event);
          this.generation = d.connectionGeneration;
          this.auth = { connectionToken: d.connectionToken, connectionGeneration: this.generation };
          this.cursor = event.id; switched = true; break;
        }
        const gen = event.data?.connectionGeneration;
        if (gen === undefined || !this.generation || gen === this.generation) await this.onEvent(event);
        this.cursor = event.id;
        if (this.stopped) return;
      }
      if (!switched) this.cursor = result.cursor;
    }
  }
  send(signal, generation) {
    if (generation !== this.generation || !this.auth?.connectionToken || this.stopped)
      return Promise.reject(new SignalError("generation"));
    const signalId = this.nextSignal++;
    const body = { code: this.code, ...this.auth, signalId, signal };
    const task = this.signalTail.then(() => this.post("web/signal", body));
    this.signalTail = task.catch(() => {});
    return task;
  }
  async leave() {
    if (!this.auth || this.stopped) return;
    // Stop only the poll now; leave still uses the authenticated request object.
    const body = { code: this.code, ...this.auth };
    this.auth = null;
    for (const controller of this.pending) controller.abort();
    try { await this.post("web/leave", body); }
    catch (error) { if (!["expired", "closed", "request_state"].includes(error.code)) throw error; }
    finally { this.close(); }
  }
  close() {
    this.stopped = true; this.auth = null;
    for (const controller of this.pending) controller.abort();
    this.pending.clear();
  }
}
