"""Headless real Chromium <-> roomc-built probe; only localhost host ICE candidates.

python tests/livemix017/r1/run_browser.py --probe build/vs2022/LiveMixWebGuestProbe_artefacts/Release/LiveMixWebGuestProbe.exe
"""
import argparse
import asyncio
import json
from pathlib import Path
import subprocess
import sys
import traceback


async def run(args, evidence):
    from playwright.async_api import async_playwright

    process = None
    browser = None
    tasks = []
    stdout_lines, stderr_lines = [], []
    evidence.update(host=None, browser=None, chrome_version=None, ok=False)
    async with async_playwright() as playwright:
        try:
            # Do not retry access-denied launches with weaker sandbox settings.
            browser = await playwright.chromium.launch(
                headless=True, channel=args.channel, chromium_sandbox=True)
            evidence["chrome_version"] = browser.version
            process = await asyncio.create_subprocess_exec(
                str(args.probe.resolve()), "--packets", str(args.packets), "--generation", "1",
                stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE,
                creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0)
            page = await browser.new_page()
            pipe_lock = asyncio.Lock()

            async def send(message):
                async with pipe_lock:
                    process.stdin.write((json.dumps(message) + "\n").encode())
                    await process.stdin.drain()

            async def binding(_source, message):
                await send(message)

            await page.expose_binding("signalToHost", binding)
            await page.goto(Path(__file__).with_name("channel.html").resolve().as_uri())
            await page.evaluate("config => probe.start(config)", {"packets": args.packets, "generation": 1})
            progress = {"sent": False, "received": False, "result": None, "eof": False}

            async def read_stdout():
                while line := await process.stdout.readline():
                    text = line.decode("utf-8", errors="replace").strip()
                    stdout_lines.append(text)
                    message = json.loads(text)
                    kind = message.get("type")
                    if kind in ("description", "candidate", "endOfCandidates"):
                        await page.evaluate("message => probe.signal(message)", message)
                    elif kind in ("sent", "received"):
                        progress[kind] = message["packets"] == args.packets
                    elif kind == "result":
                        progress["result"] = message
                    elif kind == "error" or (kind == "state" and message["state"] == "failed"):
                        raise RuntimeError(f"probe failed: {message}")
                progress["eof"] = True

            async def read_stderr():
                while line := await process.stderr.readline():
                    stderr_lines.append(line.decode("utf-8", errors="replace").rstrip())

            tasks = [asyncio.create_task(read_stdout()), asyncio.create_task(read_stderr())]
            finished = False

            def real_errors(stats):
                # After "finish" the probe closes its PeerConnection; Chrome reports that remote SCTP abort as a
                # channel error ("User-Initiated Abort"). That is the expected teardown, not a transfer failure.
                return [e for e in stats["errors"] if not (finished and "User-Initiated Abort" in e)]

            while progress["result"] is None:
                for task in tasks:
                    if task.done() and not task.cancelled() and task.exception():
                        raise task.exception()
                stats = await page.evaluate("probe.status()")
                evidence["browser"] = stats
                if real_errors(stats) or stats["mismatches"]:
                    raise RuntimeError(f"browser validation failed: {stats}")
                if (not finished and stats["sendDone"] and stats["received"] == args.packets
                        and progress["sent"] and progress["received"]):
                    await send({"type": "finish", "generation": 1})
                    finished = True
                if progress["eof"] and progress["result"] is None:
                    raise RuntimeError("probe exited without result JSON")
                await asyncio.sleep(0.05)
            evidence["host"] = progress["result"]
            evidence["probe_exit"] = await process.wait()
            stats = await page.evaluate("probe.status()")
            evidence["browser"] = stats
            evidence["teardown_errors"] = [e for e in stats["errors"] if e not in real_errors(stats)]
            negotiation = stats["negotiation"] or {}
            evidence["ok"] = (
                evidence["host"]["ok"] and evidence["probe_exit"] == 0
                and stats["sent"] == stats["received"] == args.packets
                and stats["mismatches"] == 0 and not real_errors(stats)
                and negotiation.get("label") == "livemix-audio-v2"
                and negotiation.get("protocol") == "livemix-pcm-v2"
                and negotiation.get("ordered") is False and negotiation.get("maxRetransmits") == 0
                and negotiation.get("maxPacketLifeTime") is None and negotiation.get("negotiated") is False
                and stats["localCandidates"] and stats["remoteCandidates"]
                and all(candidate == "host" for candidate in stats["localCandidates"] + stats["remoteCandidates"]))
        finally:
            if process and process.returncode is None:
                process.terminate()
                await process.wait()
            for task in tasks:
                if not task.done():
                    task.cancel()
            if tasks:
                await asyncio.gather(*tasks, return_exceptions=True)
            if browser:
                await browser.close()
            args.output.mkdir(parents=True, exist_ok=True)
            (args.output / "probe.jsonl").write_text("\n".join(stdout_lines), encoding="utf-8")
            (args.output / "probe-stderr.txt").write_text("\n".join(stderr_lines), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--packets", type=int, default=64)
    parser.add_argument("--channel", choices=("chrome", "msedge"), default=None,
                        help="omit for Playwright Chromium")
    parser.add_argument("--timeout", type=float, default=45)
    parser.add_argument("--output", type=Path, default=Path("build/r1-browser"))
    args = parser.parse_args()
    evidence = {"ok": False}
    try:
        if not args.probe.is_file():
            raise FileNotFoundError(f"Fetch the roomc-built probe first: {args.probe}")
        if not 1 <= args.packets <= 10000:
            raise ValueError("--packets must be 1..10000")
        asyncio.run(asyncio.wait_for(run(args, evidence), timeout=args.timeout))
    except (Exception, KeyboardInterrupt) as error:
        evidence["ok"] = False
        evidence["error"] = f"{type(error).__name__}: {error}"
        evidence["traceback"] = traceback.format_exc()
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "result.json").write_text(json.dumps(evidence, indent=2, ensure_ascii=False), encoding="utf-8")
    print("| Check | Result |")
    print("|---|---|")
    print(f"| Chromium | {evidence.get('chrome_version', 'not launched')} |")
    host = evidence.get("host") or {}
    browser = evidence.get("browser") or {}
    print(f"| Browser negotiation | {json.dumps(browser.get('negotiation'), ensure_ascii=False)} |")
    print(f"| Host stereo24 sent / mono16 received | {host.get('sent', '-')} / {host.get('received', '-')} |")
    print(f"| Browser mono16 sent / stereo24 received | {browser.get('sent', '-')} / {browser.get('received', '-')} |")
    print(f"| Content mismatches, host / browser | {host.get('mismatches', '-')} / {browser.get('mismatches', '-')} |")
    print(f"| Result | {'PASS' if evidence['ok'] else 'FAIL'} |")
    if "error" in evidence:
        print(evidence["error"], file=sys.stderr)
    print(f"Evidence: {args.output.resolve()}")
    return 0 if evidence["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
