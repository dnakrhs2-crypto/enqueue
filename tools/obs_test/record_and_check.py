# -*- coding: utf-8 -*-
"""Records the LiveMix plugin's output in the portable test OBS and looks for clicks / dropouts.

    python record_and_check.py --seconds 300 --ppm 200 [--writer "--restart-at 120"]
    python record_and_check.py --analyze some.mkv --ppm 200       # only analyse an existing recording

Needs the test OBS running (portable_obs.py start). Every other audio input in the scene collection is muted so the
recording holds only the plugin. The fake writer (fake_writer.py) plays a 997 Hz tone at the given clock error.
Analysis: 10 ms blocks, each least-squares fitted to a sine at the locally measured frequency; a block whose residual
is above -40 dB of the tone (the resampler itself is near -80 dB) or whose level falls below half is a fault.
"""
import argparse, json, os, subprocess, sys, tempfile, time
import numpy as np
import soundfile as sf

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from obsws import Obs  # noqa: E402

SOURCE_KIND = "livemix_master_source"
SOURCE_NAME = "LiveMix Test Source"


def prepare(obs):
    scene = obs.call("GetCurrentProgramScene")["currentProgramSceneName"]
    names = [i["inputName"] for i in obs.call("GetInputList")["inputs"]]
    if SOURCE_NAME not in names:
        obs.call("CreateInput", {"sceneName": scene, "inputName": SOURCE_NAME, "inputKind": SOURCE_KIND,
                                 "inputSettings": {}, "sceneItemEnabled": True})
    for i in obs.call("GetInputList")["inputs"]:
        obs.call("SetInputMute", {"inputName": i["inputName"], "inputMuted": i["inputName"] != SOURCE_NAME}, check=False)


NATIVE_WRITER = os.path.join(HERE, "native_writer", "build", "Release", "livemix-fake-writer.exe")


def writer_command(seconds, writer_args):
    """The native writer (MMCSS thread + high-resolution timer, like LiveMix's audio callback) when it is built; the
    Python writer only as a fallback - a busy CPU starves a Python loop (27 Sep: 17 minutes of 2.5 s gaps)."""
    if os.path.exists(NATIVE_WRITER):
        ring_env = os.environ.get("LMOBS_TEST_RING", "")
        ring = "Local\\LiveMix.ObsAudio.v1" if ring_env.lower() == "real" else (ring_env or "Local\\LiveMix.ObsAudio.test")
        return [NATIVE_WRITER, "--ring", ring, "--seconds", str(seconds)] + writer_args
    return [sys.executable, os.path.join(HERE, "fake_writer.py"), "--seconds", str(seconds)] + writer_args


def record(seconds, writer_args):
    writer = subprocess.Popen(writer_command(seconds + 8, writer_args), creationflags=subprocess.CREATE_NO_WINDOW)
    try:
        with Obs() as obs:
            prepare(obs)
            time.sleep(3.0)                      # connect + prefill before the recording starts
            obs.call("StartRecord")
            time.sleep(seconds)
            path = obs.call("StopRecord")["outputPath"]
    finally:
        writer.wait(timeout=seconds + 60)
    time.sleep(1.0)
    return path


def extract(path):
    # one file per call: two soaks ending a segment at the same moment once shared a fixed name and analysed each
    # other's half-written audio (14:42 run: 720 s and 19577 false faults)
    fd, wav = tempfile.mkstemp(prefix="livemix_obs_check_%d_" % os.getpid(), suffix=".wav")
    os.close(fd)
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", path, "-vn", "-acodec", "pcm_f32le", wav],
                   check=True, creationflags=subprocess.CREATE_NO_WINDOW)
    return wav


def analyze(wav, level=0.25, skip=1.0, block_ms=10.0):
    info = sf.info(wav)
    fs = info.samplerate
    n = int(fs * block_ms / 1000.0)
    faults, blocks, worst = [], 0, -200.0
    chunk = fs * 30                      # a whole number of blocks: the phase check runs across chunks
    t_off = 0
    prev_phase = None                    # the last block's fitted phase: a skip or a repeat at a block edge
    max_jump = 0.0                       # leaves every block clean on its own, only the phase gives it away
    with sf.SoundFile(wav) as f:
        while True:
            x = f.read(chunk, dtype="float64", always_2d=True)
            if len(x) == 0:
                break
            x = x[:, 0]
            # local frequency: FFT peak + parabolic refinement over this chunk
            w = np.hanning(len(x))
            spec = np.abs(np.fft.rfft(x * w))
            k = int(np.argmax(spec[1:-1])) + 1
            a, b, c = np.log(spec[k - 1] + 1e-30), np.log(spec[k] + 1e-30), np.log(spec[k + 1] + 1e-30)
            freq = (k + 0.5 * (a - c) / (a - 2 * b + c)) * fs / len(x)
            for i in range(0, len(x) - n + 1, n):
                t = (t_off + i) / fs
                if t < skip:
                    continue
                seg = x[i:i + n]
                tt = np.arange(n) / fs
                basis = np.stack([np.sin(2 * np.pi * freq * tt), np.cos(2 * np.pi * freq * tt), np.ones(n)], axis=1)
                coef, *_ = np.linalg.lstsq(basis, seg, rcond=None)
                fit = basis @ coef
                amp = float(np.hypot(coef[0], coef[1]))
                resid = float(np.sqrt(np.mean((seg - fit) ** 2)))
                rel = 20 * np.log10(resid / (level / np.sqrt(2)) + 1e-12)
                phase = float(np.arctan2(coef[1], coef[0]))   # seg = A sin(w t + phase)
                jump = 0.0
                if prev_phase is not None:
                    expected = prev_phase + 2 * np.pi * freq * n / fs
                    jump = abs((phase - expected + np.pi) % (2 * np.pi) - np.pi)
                prev_phase = phase
                blocks += 1
                worst = max(worst, rel)
                max_jump = max(max_jump, jump)
                if amp < level * 0.5 or rel > -40.0 or jump > 0.05:
                    faults.append({"t": round(t, 3), "amp": round(amp, 4), "resid_db": round(rel, 1),
                                   "phase_jump": round(jump, 3)})
            t_off += len(x)
    return {"seconds": round(t_off / fs, 1), "rate": fs, "blocks": blocks, "faults": len(faults),
            "first_faults": faults[:20], "worst_resid_db": round(worst, 1), "max_phase_jump": round(max_jump, 4)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=60)
    ap.add_argument("--ppm", type=float, default=0.0)
    ap.add_argument("--writer", default="", help="extra fake_writer.py arguments")
    ap.add_argument("--analyze", default="", help="analyse this recording instead of recording")
    a = ap.parse_args()
    path = a.analyze or record(a.seconds, ["--ppm", str(a.ppm)] + a.writer.split())
    wav = extract(path)
    try:
        result = analyze(wav)
    finally:
        os.remove(wav)
    result.update({"recording": path, "ppm": a.ppm})
    print(json.dumps(result, ensure_ascii=False, indent=1))


if __name__ == "__main__":
    main()
