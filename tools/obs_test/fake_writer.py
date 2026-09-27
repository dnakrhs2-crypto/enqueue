# -*- coding: utf-8 -*-
"""Pretends to be LiveMix: writes a stereo test tone into the LiveMix -> OBS ring at a deliberately wrong clock.

    python fake_writer.py --seconds 300 --ppm 200 [--rate 48000] [--block 256] [--freq 997]
                          [--restart-at 120] [--rate-change-at 200:44100] [--stall-at 60:3]

The tone is written against the writer's own frame counter while the frames are paced at rate*(1+ppm/1e6) per real
second, i.e. like an audio interface whose crystal runs ppm fast. Layout = livemix/shared/lm_obs_protocol.h (v1):
header 256 bytes (epoch @32, sample_rate @40, write_frames @48, heartbeat_qpc @56, qpc_frequency @64,
send_enabled @72, writer_pid @80, silent_frames @88, in-flight end @96), PCM float32 interleaved stereo after it.
The mapping name is the real one, so a running LiveMix must not be sending at the same time.
"""
import argparse, ctypes, math, mmap, os, struct, sys, time

NAME = os.environ.get("LMOBS_TEST_RING") or "Local\\LiveMix.ObsAudio.v1"   # a test OBS can read another ring
MAGIC, MAJOR, MINOR = 0x424F4D4C, 1, 0
HEADER, CAPACITY, CHANNELS = 256, 32768, 2
OFF = dict(epoch=32, sample_rate=40, write_frames=48, heartbeat_qpc=56, qpc_frequency=64,
           send_enabled=72, writer_pid=80, silent_frames=88, inflight=96)

k32 = ctypes.windll.kernel32


def qpc():
    v = ctypes.c_int64()
    k32.QueryPerformanceCounter(ctypes.byref(v))
    return v.value


def qpf():
    v = ctypes.c_int64()
    k32.QueryPerformanceFrequency(ctypes.byref(v))
    return v.value


class Ring:
    def __init__(self):
        size = HEADER + CAPACITY * CHANNELS * 4
        self.mm = mmap.mmap(-1, size, tagname=NAME)
        struct.pack_into("<8I", self.mm, 0, MAGIC, MAJOR, MINOR, HEADER, size, HEADER, CHANNELS, CAPACITY)
        self.i64 = {k: ctypes.c_int64.from_buffer(self.mm, o) for k, o in OFF.items()}   # aligned 8-byte stores
        self.pcm = (ctypes.c_float * (CAPACITY * CHANNELS)).from_buffer(self.mm, HEADER)
        self.set("qpc_frequency", qpf())
        self.set("writer_pid", os.getpid())

    def set(self, key, value):
        self.i64[key].value = int(value)

    def get(self, key):
        return self.i64[key].value

    def new_epoch(self, rate):
        # owner reset: stop, disable, reset both positions, then publish the new epoch
        self.set("send_enabled", 0)
        self.set("write_frames", 0)
        self.set("inflight", 0)
        self.set("sample_rate", rate)
        self.set("epoch", self.get("epoch") + 1)
        self.set("send_enabled", 1)

    def write(self, samples_lr):
        start = self.get("write_frames")
        n = len(samples_lr) // 2
        self.set("inflight", start + n)
        for i in range(n):
            j = ((start + i) & (CAPACITY - 1)) * 2
            self.pcm[j] = samples_lr[2 * i]
            self.pcm[j + 1] = samples_lr[2 * i + 1]
        self.set("write_frames", start + n)
        self.set("heartbeat_qpc", qpc())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=60)
    ap.add_argument("--ppm", type=float, default=0.0)
    ap.add_argument("--rate", type=int, default=48000)
    ap.add_argument("--block", type=int, default=256)
    ap.add_argument("--freq", type=float, default=997.0)
    ap.add_argument("--level", type=float, default=0.25)
    ap.add_argument("--restart-at", type=float, default=-1)
    ap.add_argument("--rate-change-at", default="")
    ap.add_argument("--stall-at", default="")
    ap.add_argument("--normal-priority", action="store_true",
                    help="run like an ordinary process (default: like an audio callback - high class, time-critical thread)")
    a = ap.parse_args()

    if not a.normal_priority:
        # LiveMix writes from its audio device thread, which the OS schedules ahead of builds and browsers. A plain Python
        # process gets starved by a busy CPU (13:47 run: MSBuild -> 108 receiver underruns that LiveMix would never cause).
        k32.SetPriorityClass(k32.GetCurrentProcess(), 0x00000080)          # HIGH_PRIORITY_CLASS
        k32.SetThreadPriority(k32.GetCurrentThread(), 15)                  # THREAD_PRIORITY_TIME_CRITICAL
        ctypes.windll.winmm.timeBeginPeriod(1)                             # 1 ms sleeps

    ring = Ring()
    rate = a.rate
    ring.new_epoch(rate)
    freq_hz = qpf()
    t0 = qpc()
    written = 0          # frames of this epoch
    phase = 0.0
    epoch_t0 = 0.0       # seconds since t0 when this epoch started
    rate_change = tuple(float(x) for x in a.rate_change_at.split(":")) if a.rate_change_at else None
    stall = tuple(float(x) for x in a.stall_at.split(":")) if a.stall_at else None
    restarted = changed = stalled = False
    print("writing %s: rate %d, %+.1f ppm, %.0f s" % (NAME, rate, a.ppm, a.seconds), flush=True)
    while True:
        now = (qpc() - t0) / freq_hz
        if now >= a.seconds:
            break
        if stall and not stalled and now >= stall[0]:
            stalled = True
            time.sleep(stall[1])            # the writer stops (a frozen LiveMix): no frames, no heartbeat
            epoch_t0 += stall[1]            # carry on from where it stopped, no catch-up burst
            continue
        if a.restart_at >= 0 and not restarted and now >= a.restart_at:
            restarted = True
            ring.new_epoch(rate)
            written, epoch_t0 = 0, now
        if rate_change and not changed and now >= rate_change[0]:
            changed = True
            rate = int(rate_change[1])
            ring.new_epoch(rate)
            written, epoch_t0 = 0, now
        due = int((now - epoch_t0) * rate * (1.0 + a.ppm / 1e6))
        if due - written >= a.block:
            buf = []
            step = 2.0 * math.pi * a.freq / rate
            for _ in range(a.block):
                s = a.level * math.sin(phase)
                buf += (s, s)
                phase += step
                if phase > 2.0 * math.pi:
                    phase -= 2.0 * math.pi
            ring.write(buf)
            written += a.block
        else:
            time.sleep(0.001)
    ring.set("send_enabled", 0)
    print("done: epoch %d, frames %d" % (ring.get("epoch"), ring.get("write_frames")), flush=True)


if __name__ == "__main__":
    main()
