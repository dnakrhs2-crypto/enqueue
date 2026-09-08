import { resolve } from "node:path";
import { readFile } from "node:fs/promises";
export const packageRoot = process.cwd();
export const pluginRoot = resolve(packageRoot, "com.gomtwigim.livemix.sdPlugin");
export async function fixture(name) { return JSON.parse(await readFile(resolve(packageRoot, "tests/fixtures/control", `${name}.json`), "utf8")); }
/** Resolve on an event that satisfies a condition, with an explicit failure deadline. No polling sleeps. */
export function until(emitter, event, condition, timeout = 4000) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => finish(new Error(`Deadline waiting for ${event}`)), timeout);
    const check = () => { try { if (condition()) finish(); } catch (error) { finish(error); } };
    const finish = error => { clearTimeout(timer); emitter.off(event, check); error ? reject(error) : resolve(); };
    emitter.on(event, check); check();
  });
}
export function nextEvent(emitter, event, timeout = 4000) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => { emitter.off(event, listener); reject(new Error(`Deadline waiting for ${event}`)); }, timeout);
    const listener = (...args) => { clearTimeout(timer); resolve(args); };
    emitter.once(event, listener);
  });
}
