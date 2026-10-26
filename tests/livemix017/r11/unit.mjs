import assert from "node:assert/strict";
import { readFile, writeFile, mkdir } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { createPreferences, cleanNickname, validNickname, errorText, ERROR_TEXT,
  NICKNAME_KEY, VOLUME_PREFIX } from "../../../livemix/webjoin/app.js";
import { parseInvite } from "../../../livemix/webjoin/guest-client.js";

const results = [];
async function test(name, run) {
  try { await run(); results.push({ name, ok: true }); console.log("PASS", name); }
  catch (error) { results.push({ name, ok: false, error: error.stack }); console.error("FAIL", name, error); }
}
function memory() {
  const data = new Map();
  return { data, getItem: key => data.get(key) ?? null, setItem: (key, value) => data.set(key, value) };
}

await test("R6 fragment preserves ASCII code and invite generation", () => {
  assert.deepEqual(parseInvite("#v=2&code=0042&invite=" + "ab".repeat(32) + "&generation=3"),
    { code: "0042", inviteToken: "ab".repeat(32), inviteGeneration: 3 });
  assert.throws(() => parseInvite(""));
});
await test("new device defaults to empty nickname and 100 percent", () => {
  const prefs = createPreferences(() => memory());
  assert.equal(prefs.nickname(), "");
  assert.equal(prefs.volume("하람"), 100);
});
await test("nickname and per-nickname volume survive reopening", () => {
  const storage = memory(), prefs = createPreferences(() => storage);
  prefs.rememberNickname(" 하늘 ");
  prefs.rememberVolume("하람", 35);
  prefs.rememberVolume("윤슬", 70);
  const reopened = createPreferences(() => storage);
  assert.equal(reopened.nickname(), "하늘");
  assert.equal(reopened.volume("하람"), 35);
  assert.equal(reopened.volume("윤슬"), 70);
  assert.equal(reopened.volume("새 사람"), 100);
});
await test("zero and 100 percent are remembered without truthiness fallback", () => {
  const storage = memory(), prefs = createPreferences(() => storage);
  for (const value of [0, 100]) {
    prefs.rememberVolume("하람", value);
    assert.equal(createPreferences(() => storage).volume("하람"), value);
  }
});
await test("unavailable localStorage getter is optional", () => {
  const prefs = createPreferences(() => { throw new Error("SecurityError"); });
  assert.equal(prefs.nickname(), "");
  assert.equal(prefs.volume("하람"), 100);
  assert.doesNotThrow(() => prefs.rememberNickname("하늘"));
  assert.doesNotThrow(() => prefs.rememberVolume("하람", 0));
});
await test("read and write failures are contained independently", () => {
  const storage = memory();
  storage.data.set(NICKNAME_KEY, "하늘");
  storage.data.set(VOLUME_PREFIX + "하람", "25");
  storage.setItem = () => { throw new Error("QuotaExceededError"); };
  const prefs = createPreferences(() => storage);
  assert.doesNotThrow(() => prefs.rememberVolume("하람", 50));
  assert.doesNotThrow(() => prefs.rememberNickname("다른 이름"));
  assert.equal(prefs.volume("하람"), 25);
  assert.equal(prefs.nickname(), "하늘");
  storage.getItem = () => { throw new Error("SecurityError"); };
  assert.equal(prefs.volume("하람"), 100);
  assert.equal(prefs.nickname(), "");
});
await test("invalid remembered values use 100 percent", () => {
  const storage = memory(), prefs = createPreferences(() => storage);
  for (const value of ["", " ", "-1", "101", "0.5", "NaN", "null", "{}", "undefined"]) {
    storage.data.set(VOLUME_PREFIX + "하람", value);
    assert.equal(prefs.volume("하람"), 100, value);
  }
});
await test("nicknames remain separate literal storage keys", () => {
  const storage = memory(), prefs = createPreferences(() => storage);
  prefs.rememberVolume("__proto__", 15);
  prefs.rememberVolume("constructor", 30);
  assert.equal(prefs.volume("__proto__"), 15);
  assert.equal(prefs.volume("constructor"), 30);
  assert.equal(prefs.volume("하람"), 100);
});
await test("nickname uses R6 scalar and UTF-8 limits", () => {
  assert.equal(cleanNickname(" \u0000하\n늘\u007f "), "하늘");
  assert.equal(validNickname(" "), false);
  assert.equal(validNickname("가".repeat(40)), true);
  assert.equal(validNickname("가".repeat(41)), false);
  assert.equal(validNickname("😀".repeat(40)), true);
  assert.equal(validNickname("😀".repeat(41)), false);
  const storage = memory();
  storage.data.set(NICKNAME_KEY, "가".repeat(41));
  assert.equal(createPreferences(() => storage).nickname(), "");
});
await test("error codes map to the few approved sentences", () => {
  const groups = [
    [ERROR_TEXT, ["invite", "closed", "invalidated", "expired", "ended", "error", "token", "bad_response",
      "audio", "worker", "unknown", undefined]],
    ["마이크를 쓸 수 없습니다.", ["microphone_denied", "microphone_missing", "microphone_busy",
      "microphone_constraints", "microphone", "microphone_ended", "microphone_required"]],
    ["방이 꽉 찼습니다.", ["full", "web_full"]],
    ["방장이 거절했습니다.", ["rejected"]],
    ["연결하지 못했습니다.", ["ice", "ice_candidates", "connect_timeout", "channel", "channel_config", "webrtc",
      "sdp", "network", "timeout"]],
  ];
  for (const [text, codes] of groups)
    for (const code of codes) assert.equal(errorText({ code, message: "Unapproved server text" }), text, String(code));
});
await test("production markup has local modules, no CDN or backend host text", async () => {
  const base = new URL("../../../livemix/webjoin/", import.meta.url);
  const html = await readFile(new URL("index.html", base), "utf8");
  const css = await readFile(new URL("join.css", base), "utf8");
  const js = await readFile(new URL("app.js", base), "utf8");
  assert.match(html, /<script type="module" src="\.\/app\.js"><\/script>/);
  assert.match(html, /<label class="lab" for="nickname">닉네임<\/label>/);
  assert.doesNotMatch(html + css + js, /유튜브다운\.com|곰튀김\.com|https?:\/\/|@import|@font-face/);
  assert.doesNotMatch(html + js, /상대방 음량|①|②|③|④|frame-label/);
  assert.match(css, /max-width: 420px/);
  assert.match(css, /height: 44px/);
});

const summary = { passed: results.filter(r => r.ok).length, failed: results.filter(r => !r.ok).length, tests: results };
if (process.argv[2]) {
  const output = resolve(process.argv[2]);
  const allowed = fileURLToPath(new URL("./", import.meta.url));
  assert.ok(output.startsWith(allowed), "Keep R11 results inside its owned directory");
  await mkdir(dirname(output), { recursive: true });
  await writeFile(output, JSON.stringify(summary, null, 2) + "\n");
}
console.log(JSON.stringify({ passed: summary.passed, failed: summary.failed }));
if (summary.failed) process.exitCode = 1;
