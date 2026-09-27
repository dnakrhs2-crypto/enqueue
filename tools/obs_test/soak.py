# -*- coding: utf-8 -*-
"""Long unattended run of the LiveMix OBS plugin: fake LiveMix at a wrong clock -> test OBS -> recordings checked.

    python soak.py --hours 4 --ppm 150      (start it with tools\\detach_run.py so it outlives the session)

Records in 30-minute segments (FLAC in MKV), analyses each one with record_and_check.analyze, deletes clean
recordings, keeps faulty ones, and writes %LOCALAPPDATA%\\LiveMixObsTest\\soak_report.json after every segment plus the
plugin's own [livemix-obs] statistics lines from the OBS log at the end.
"""
import argparse, json, os, shutil, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

# an isolated test OBS (its own folder, port, ring and plugin copy) is chosen before portable_obs reads the environment
_pre = argparse.ArgumentParser(add_help=False)
_pre.add_argument("--isolated", default="", help="folder name under %%LOCALAPPDATA%% for a second test OBS")
_pre.add_argument("--port", default="4467")
_pre.add_argument("--priority", default="High")
_known, _ = _pre.parse_known_args()
if _known.isolated:
    _root = os.path.join(os.environ["LOCALAPPDATA"], _known.isolated)
    _plugin = os.path.join(_root, "plugin")
    _rundir = os.path.normpath(os.path.join(HERE, "..", "..", "obs-plugin", "build_x64", "rundir", "RelWithDebInfo"))
    if os.path.isdir(_plugin):
        shutil.rmtree(_plugin)
    shutil.copytree(_rundir, _plugin, ignore=shutil.ignore_patterns("*.pdb"))
    os.environ.update(LMOBS_TEST_ROOT=_root, LMOBS_TEST_PORT=_known.port, LMOBS_TEST_PLUGIN_DIR=_plugin,
                      LMOBS_TEST_RING="Local\\LiveMix.ObsAudio." + _known.isolated, LMOBS_TEST_PRIORITY=_known.priority)

import portable_obs  # noqa: E402
from obsws import Obs  # noqa: E402
from record_and_check import prepare, extract, analyze, writer_command  # noqa: E402

REPORT = os.path.join(portable_obs.ROOT, "soak_report.json")


def save(report):
    tmp = REPORT + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=1)
    os.replace(tmp, REPORT)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hours", type=float, default=4.0)
    ap.add_argument("--ppm", type=float, default=150.0)
    ap.add_argument("--segment", type=float, default=1800.0)
    ap.add_argument("--isolated", default="")
    ap.add_argument("--port", default="4467")
    ap.add_argument("--priority", default="High")
    a = ap.parse_args()

    total = a.hours * 3600.0
    report = {"started": time.strftime("%Y-%m-%d %H:%M:%S"), "hours": a.hours, "ppm": a.ppm,
              "segments": [], "faults_total": 0, "state": "running"}
    save(report)
    portable_obs.stop()
    portable_obs.setup()
    portable_obs.start()
    # the writer's own 5 s lines (written vs due, largest wake-up gap, late blocks) go to writer.log next to the report
    writer_log = open(os.path.join(portable_obs.ROOT, "writer.log"), "w", encoding="utf-8")
    writer = subprocess.Popen(writer_command(total + 120, ["--ppm", str(a.ppm)]), stdout=writer_log,
                              stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
    try:
        with Obs() as obs:
            prepare(obs)
        time.sleep(5.0)
        done = 0.0
        while done < total:
            length = min(a.segment, total - done)
            with Obs() as obs:
                obs.call("StartRecord")
            time.sleep(length)
            with Obs() as obs:
                path = obs.call("StopRecord")["outputPath"]
            time.sleep(2.0)
            wav = extract(path)
            try:
                result = analyze(wav)
            finally:
                os.remove(wav)
            result["file"] = path
            result["at_hours"] = round((done + length) / 3600.0, 2)
            report["segments"].append(result)
            report["faults_total"] += result["faults"]
            if result["faults"] == 0:
                os.remove(path)
            save(report)
            done += length
        report["state"] = "done"
    except Exception as e:  # keep what was measured
        report["state"] = "error: %r" % (e,)
    finally:
        writer.kill()
        logdir = os.path.join(portable_obs.CFG, "logs")
        logs = sorted((os.path.join(logdir, f) for f in os.listdir(logdir)), key=os.path.getmtime)
        with open(logs[-1], encoding="utf-8", errors="replace") as f:
            report["plugin_stats"] = [line.strip() for line in f if "[livemix-obs]" in line][-60:]
        report["finished"] = time.strftime("%Y-%m-%d %H:%M:%S")
        save(report)
        portable_obs.stop()
    print(json.dumps({k: report[k] for k in ("state", "faults_total")}, ensure_ascii=False))


if __name__ == "__main__":
    main()
