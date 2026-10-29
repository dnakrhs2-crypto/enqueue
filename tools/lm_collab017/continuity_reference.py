# Frozen continuity from 5545f809; used only by the chunk-equivalence test.
import numpy as np
from audio import TONES, LEVEL

def original_continuity(pcm: np.ndarray, rate: int, voice: int, gain: float = 1) -> dict:
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
