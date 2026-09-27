# -*- coding: utf-8 -*-
"""Runs the short real-OBS checks one after another and prints one line per case (needs the test OBS running)."""
import json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
CASES = [
    ("source +200 ppm", ["--seconds", "60", "--ppm", "200"]),
    ("source -200 ppm", ["--seconds", "60", "--ppm", "-200"]),
    ("source restart@20 stall@40:3", ["--seconds", "60", "--ppm", "100",
                                      "--writer", "--restart-at 23 --stall-at 43:3"]),
    ("source 44.1k->48k@25", ["--seconds", "60", "--ppm", "0",
                              "--writer", "--rate 44100 --rate-change-at 28:48000"]),
]
only = sys.argv[1:]
for name, args in CASES:
    if only and not any(o in name for o in only):
        continue
    out = subprocess.run([sys.executable, os.path.join(HERE, "record_and_check.py")] + args, capture_output=True,
                         text=True, encoding="utf-8", errors="replace", creationflags=subprocess.CREATE_NO_WINDOW)
    try:
        r = json.loads(out.stdout[out.stdout.index("{"):])
        print("%-30s faults=%d worst=%.1f dB jump=%.3f first=%s" % (
            name, r["faults"], r["worst_resid_db"], r["max_phase_jump"],
            [(f["t"], f.get("amp"), f.get("phase_jump")) for f in r["first_faults"][:6]]), flush=True)
    except ValueError:
        print("%-30s ERROR %s" % (name, (out.stdout + out.stderr)[-400:]), flush=True)
