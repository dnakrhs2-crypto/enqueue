# -*- coding: utf-8 -*-
"""Long unattended run of the LiveMix OBS plugin: fake LiveMix at a wrong clock -> test OBS -> recordings checked.

    python soak.py --hours 4 --ppm 150 --mode source      (start it with tools\\detach_run.py so it outlives the session)

Records in 30-minute segments (FLAC in MKV), analyses each one with record_and_check.analyze, deletes clean
recordings, keeps faulty ones, and writes %LOCALAPPDATA%\\LiveMixObsTest\\soak_report.json after every segment plus the
plugin's own [livemix-obs] statistics lines from the OBS log at the end.
"""
import argparse, json, os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import portable_obs  # noqa: E402
from obsws import Obs  # noqa: E402
from record_and_check import prepare, extract, analyze  # noqa: E402

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
    ap.add_argument("--mode", choices=["source", "filter"], default="source")
    ap.add_argument("--segment", type=float, default=1800.0)
    a = ap.parse_args()

    total = a.hours * 3600.0
    report = {"started": time.strftime("%Y-%m-%d %H:%M:%S"), "hours": a.hours, "ppm": a.ppm, "mode": a.mode,
              "segments": [], "faults_total": 0, "state": "running"}
    save(report)
    portable_obs.stop()
    portable_obs.setup()
    portable_obs.start()
    writer = subprocess.Popen([sys.executable, os.path.join(HERE, "fake_writer.py"), "--seconds", str(total + 120),
                               "--ppm", str(a.ppm)], creationflags=subprocess.CREATE_NO_WINDOW)
    try:
        with Obs() as obs:
            prepare(obs, a.mode)
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
            result = analyze(extract(path))
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
