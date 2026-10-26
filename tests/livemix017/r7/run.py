"""Run the built plugin in libobs without launching OBS or using audio hardware."""
import argparse
import os
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--configuration", default="RelWithDebInfo")
    args = parser.parse_args()
    plugin = Path(__file__).resolve().parents[3] / "obs-plugin"
    binary = plugin / "build_x64" / args.configuration
    env = os.environ.copy()
    env["PATH"] = os.pathsep.join([
        str(plugin / ".deps/bin/64bit"),
        str(plugin / ".deps/obs-deps-2025-07-11-x64/bin"),
        env.get("PATH", ""),
    ])
    return subprocess.run([
        str(binary / "livemix-obs-peer-source-tests.exe"),
        str(binary / "livemix-obs.dll"),
        str(plugin / "data"),
    ], env=env, check=False).returncode


if __name__ == "__main__":
    sys.exit(main())
