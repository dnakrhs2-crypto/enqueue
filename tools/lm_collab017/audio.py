"""Independent PCM analysis. Values are measurements, never expected constants filled in as results."""
from __future__ import annotations

import numpy as np

TONES = ((700, 1100), (1700, 2300), (2900, 3100), (3700, 3700))
MARKERS = (6000, 8000, 10000, 12000)
LEVEL = .12


def signal(rate: int, frames: int, voice: int) -> np.ndarray:
    f = np.arange(frames, dtype=np.float64)
    result = LEVEL * np.sin(2 * np.pi * f[:, None] * np.array(TONES[voice]) / rate)
    phase = f % (rate // 2)
    length = round(rate * .002)
    selected = phase < length
    p = phase[selected]
    burst = .6 * (.5 - .5 * np.cos(2 * np.pi * p / (length - 1))) * np.sin(2 * np.pi * MARKERS[voice] * p / rate)
    result[selected] += burst[:, None]
    return result.astype('<f4')


def amplitudes(pcm: np.ndarray, rate: int) -> list[list[list[float]]]:
    """Median of 1 s Hann spectra, interpolated by zero padding. Tolerates clock drift."""
    if len(pcm) < rate // 2:
        raise ValueError('need at least 500 ms of measured audio')
    size = min(rate, len(pcm))
    starts = np.linspace(0, len(pcm) - size, min(5, max(1, len(pcm) // size)), dtype=int)
    w = np.hanning(size)
    nfft = 1 << (size * 4 - 1).bit_length()
    spectra = [2 * abs(np.fft.rfft(pcm[s:s+size] * w[:, None], n=nfft, axis=0)) / w.sum() for s in starts]
    def at(hz, ch):
        lo, hi = int((hz - 5) * nfft / rate), int((hz + 5) * nfft / rate) + 1
        # Track each window's peak BEFORE taking the median. Clock adaptation can
        # move that peak between FFT bins without changing the received level.
        return float(np.median([s[lo:hi, ch].max() for s in spectra]))
    return [[[at(hz, ch) for hz in tones] for ch in range(2)] for tones in TONES]


def continuity(pcm: np.ndarray, rate: int, voice: int, gain: float = 1) -> dict:
    size = round(rate * .01)
    count = len(pcm) // size
    if count < 2:
        raise ValueError('continuity interval too short')
    # Same 10 ms analysis cadence as the original CollabProbe, independently for L/R.
    x = np.asarray(pcm[:count*size], dtype=np.float64).reshape(count, size, 2)
    f = np.arange(size)
    phasor = np.exp(-2j*np.pi*f[:, None]*np.array(TONES[voice])/rate)
    z = 2 * (x * phasor).sum(axis=1) / size
    level = abs(z) / (LEVEL * max(gain, 1e-12))
    low = np.any(level < .7, axis=1)
    high = np.any(level > 1.3, axis=1)
    quiet = np.all(level < .1, axis=1)
    # Remove nominal phase advance for the actual analysis window length.
    delta = np.angle(z[1:] * np.conj(z[:-1]) * np.exp(-2j*np.pi*np.array(TONES[voice])*size/rate))
    jump = np.any(abs(delta) > .35, axis=1) & ~low[1:] & ~low[:-1]
    bad = low | high; bad[1:] |= jump
    starts = np.flatnonzero(bad & ~np.r_[False, bad[:-1]])
    ends = np.flatnonzero(bad & ~np.r_[bad[1:], False]) + 1
    return {'windows': count, 'windowMs': 1000*size/rate, 'levelDropWindows': int(low.sum()),
            'quietWindows': int(quiet.sum()), 'highLevelWindows': int(high.sum()), 'phaseJumpWindows': int(jump.sum()),
            'events': len(starts), 'longestMs': float(max(ends-starts, default=0)*size*1000/rate),
            'minLevelRatio': float(level.min()), 'medianLevelRatio': float(np.median(level)),
            'maxPhaseRadians': float(abs(delta).max()), 'eventStartMs': (starts*size*1000/rate).tolist()}


def matrix(pcm: np.ndarray, rate: int, present: list[int], *, mono: bool = False) -> dict:
    measured = amplitudes(pcm, rate)
    checks = []
    for voice in range(4):
        for ch in range(2):
            wanted = measured[voice][ch][ch]
            checks.append({'voice': voice, 'channel': ch, 'amplitude': wanted,
                           'expected': voice in present, 'ok': .084 < wanted < .156 if voice in present else wanted < .0024})
    separation = {}
    for voice in present:
        if TONES[voice][0] != TONES[voice][1]:
            separation[str(voice)] = [float(20*np.log10(max(measured[voice][ch][ch], 1e-12) /
                                      max(measured[voice][1-ch][ch], 1e-12))) for ch in range(2)]
    mono_error = float(np.max(abs(pcm[:, 0] - pcm[:, 1]))) if mono else None
    return {'ok': all(c['ok'] for c in checks) and all(min(v) >= 35 for v in separation.values())
                  and (not mono or mono_error < 2e-5),
            'tones': measured, 'checks': checks, 'separationDb': separation, 'monoMaxDifference': mono_error,
            'continuity': {str(v): continuity(pcm, rate, v) for v in present}}


def marker_positions(pcm: np.ndarray, rate: int, voice: int) -> list[int]:
    x = np.asarray(pcm[:, 0], dtype=np.float64)
    if not len(x):
        return []
    n = 1 << (len(x)-1).bit_length()
    spec = np.fft.rfft(x, n)
    freq = np.fft.rfftfreq(n, 1/rate)
    spec[abs(freq - MARKERS[voice]) > 900] = 0
    filtered = np.fft.irfft(spec, n)[:len(x)]
    energy = np.convolve(filtered**2, np.ones(max(1, round(rate*.0005)))/max(1, round(rate*.0005)), 'same')
    indices = np.flatnonzero(energy > .012)
    groups = np.split(indices, np.flatnonzero(np.diff(indices) > rate*.05) + 1)
    return [int(g[np.argmax(energy[g])]) for g in groups if len(g)]


def roundtrip(local: np.ndarray, returned: np.ndarray, rate: int) -> dict:
    sent, heard = marker_positions(local, rate, 0), marker_positions(returned, rate, 0)
    delays = []
    for f in heard:
        preceding = [s for s in sent if 0 < f-s < .45*rate]
        if preceding:
            delays.append((f-preceding[-1])*1000/rate)
    return {'ok': len(delays) >= 3, 'sentMarkers': sent, 'returnedMarkers': heard,
            'count': len(delays), 'medianMs': float(np.median(delays)) if delays else None,
            'p95Ms': float(np.percentile(delays, 95)) if delays else None,
            'minMs': min(delays, default=None), 'maxMs': max(delays, default=None), 'samplesMs': delays,
            'method': 'same host callback clock; 2ms marker every 500ms; unambiguous range 0..450ms; no device latency'}


def quantize24(x: np.ndarray) -> np.ndarray:
    scaled = np.asarray(x, dtype=np.float64) * 8388608
    return np.clip(np.copysign(np.floor(abs(scaled) + .5), scaled), -8388608, 8388607).astype(np.int32)


def align_exact(reference: np.ndarray, recorded: np.ndarray, lower: int, upper: int, rate: int) -> dict:
    """Find the start by correlation, then test every stereo sample against callback PCM24."""
    n = min(len(recorded), rate)
    lower = max(0, lower)
    upper = min(upper, len(reference)-len(recorded))
    if n < 100 or upper < lower:
        return {'ok': False, 'reason': 'capture does not cover recording'}
    ref = quantize24(reference[lower:upper+n+1])
    a, b = ref[:, 0].astype(float), recorded[:n, 0].astype(float)
    size = 1 << (len(a)+n-2).bit_length()
    corr = np.fft.irfft(np.fft.rfft(a, size) * np.fft.rfft(b[::-1], size), size)[n-1:len(a)]
    squares = np.r_[0., np.cumsum(a*a)]
    error = squares[n:] - squares[:-n] + np.dot(b, b) - 2*corr
    candidates = np.argsort(error)[:8]
    best = None
    for candidate in candidates:
        offset = lower + int(candidate)
        expected = quantize24(reference[offset:offset+len(recorded)])
        mismatch = np.any(expected != recorded, axis=1)
        item = {'ok': not bool(mismatch.any()), 'startFrame': offset,
                'mismatchedFrames': int(mismatch.sum()), 'maxSampleError': int(abs(expected.astype(np.int64)-recorded).max())}
        if best is None or item['mismatchedFrames'] < best['mismatchedFrames']:
            best = item
        if item['ok']:
            break
    return best
