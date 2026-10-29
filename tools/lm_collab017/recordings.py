"""Read every output file, including actual PCM24 integers and independently decoded MP3."""
from __future__ import annotations

import json
from pathlib import Path
import re
import subprocess
import wave

import numpy as np
from audio import align_exact, amplitudes, marker_positions, matrix

HIDDEN = (subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS) if __import__('sys').platform == 'win32' else 0


def read_wav(path: Path):
    with wave.open(str(path), 'rb') as f:
        meta = {'rate': f.getframerate(), 'channels': f.getnchannels(), 'bits': f.getsampwidth()*8, 'frames': f.getnframes()}
        if meta['channels'] != 2 or meta['bits'] != 24:
            raise ValueError('expected stereo PCM24')
        data = np.frombuffer(f.readframes(f.getnframes()), dtype=np.uint8)
    if len(data) != meta['frames'] * 6:
        raise ValueError('WAV header exceeds readable PCM')
    b = data.reshape(-1, 3).astype(np.int32)
    pcm = b[:, 0] | b[:, 1] << 8 | b[:, 2] << 16
    pcm = ((pcm ^ 0x800000) - 0x800000).reshape(-1, 2)
    meta.update(durationSeconds=len(pcm)/meta['rate'], bitrateKbps=meta['rate']*48/1000, bytes=path.stat().st_size)
    return meta, pcm


def mp3_headers(path: Path):
    data = path.read_bytes(); pos = frames = 0; rates, bitrates = set(), set()
    while pos + 4 <= len(data):
        a,b,c,d = data[pos:pos+4]
        if a != 255 or b & 0xfe != 0xfa or d >> 6 == 3:
            raise ValueError(f'invalid stereo MPEG1 layer3 header at {pos}')
        rate = (44100,48000,32000,0)[(c>>2)&3]
        kbps = (0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0)[c>>4]
        if not rate or not kbps:
            raise ValueError('invalid MP3 rate/bitrate')
        pos += 144000*kbps//rate + ((c>>1)&1); frames += 1152
        rates.add(rate); bitrates.add(kbps)
    if pos != len(data) or len(rates) != 1 or len(bitrates) != 1 or not frames:
        raise ValueError('truncated or inconsistent MP3')
    rate = rates.pop()
    return {'rate': rate, 'bitrateKbps': bitrates.pop(), 'mpegFrames': frames, 'durationSeconds': frames/rate, 'bytes': len(data)}


def inspect_take(take: dict, participant: dict, folder: Path, probe: Path, expected: dict[str, int], split_limit=0):
    result = {'label': take['label'], 'ok': True, 'files': [], 'checks': [], 'cpu': {k:v for k,v in take.items() if 'cpu' in k.lower() or k == 'wallSeconds'}}
    def check(label, ok, **values):
        result['checks'].append({'check':label, 'ok':bool(ok), **values})
        result['ok'] &= bool(ok)
    is_wav = take['format'] == 'wav'
    groups = {}
    for file in sorted(Path(take['directory']).rglob('*.wav' if is_wav else '*.mp3')):
        found = re.match(r'^([ABCD])_(.*?)(?:_(\d+))?\.(wav|mp3)$', file.name)
        if not found:
            raise ValueError(f'unexpected recording name: {file.name}')
        role, _, part, _ = found.groups(); part = int(part or 1)
        if is_wav:
            meta, pcm = read_wav(file)
        else:
            meta = mp3_headers(file)
            decoded = folder / f"decoded-{take['label']}-{role}-{part}.f32"
            completed = subprocess.run([str(probe), '--decode', str(file), str(decoded)], capture_output=True, timeout=30, creationflags=HIDDEN)
            if completed.returncode:
                raise RuntimeError(completed.stderr.decode(errors='replace'))
            decoded_meta = json.loads(decoded.with_suffix('.json').read_text(encoding='utf-8-sig'))
            pcm = np.fromfile(decoded, '<f4').reshape(-1, 2)
            meta.update(decodedFrames=len(pcm), decodedRate=decoded_meta['rate'])
            check(f'{file.name}: decoder length/rate', len(pcm) == decoded_meta['frames'] and decoded_meta['rate'] == meta['rate'],
                  decodedFrames=len(pcm), declaredFrames=decoded_meta['frames'], decodedRate=decoded_meta['rate'])
        meta.update(file=str(file), role=role, part=part)
        result['files'].append(meta)
        groups.setdefault(role, []).append((part, pcm, meta))
        expected_rate = participant['rate'] if is_wav or participant['rate'] in (32000,44100,48000) else 48000
        check(f'{file.name}: rate', meta['rate'] == expected_rate, actual=meta['rate'], expected=expected_rate)
        if not is_wav:
            check(f'{file.name}: bitrate', meta['bitrateKbps'] == take['kbps'], actual=meta['bitrateKbps'], expected=take['kbps'])
        if split_limit:
            check(f'{file.name}: whole-file limit', meta['bytes'] <= split_limit, bytes=meta['bytes'], limit=split_limit)
    check('file roles / empty seat deletion', set(groups) == set(expected) | {'D'}, actual=sorted(groups), expected=sorted(set(expected)|{'D'}))
    if not groups or 'A' not in groups or 'D' not in groups:
        return result
    audio = {}
    rate = groups['A'][0][2]['rate']
    for role, parts in groups.items():
        parts.sort(key=lambda p:p[0])
        check(f'{role}: sequential parts', [p[0] for p in parts] == list(range(1,len(parts)+1)))
        audio[role] = np.concatenate([p[1] for p in parts])
        check(f'{role}: same split boundaries', [len(p[1]) for p in parts] == [len(p[1]) for p in groups['A']])
    check('same decoded lengths', len({len(p) for p in audio.values()}) == 1,
          frames={k:len(v) for k,v in audio.items()})
    if split_limit:
        check('size split exercised', len(groups['A']) > 1, parts=len(groups['A']))
    expected_frames = take['snapshot']['capturedFrames']
    if is_wav:
        check('capture length', len(audio['A']) == expected_frames, actual=len(audio['A']), expected=expected_frames)
        total = sum((audio[k].astype(np.int64) for k in ('A','B','C') if k in audio), np.zeros_like(audio['A'], dtype=np.int64))
        if len(audio['D']) == len(total):
            mismatch = audio['D'] != np.clip(total, -8388608, 8388607)
            check('D = saturated PCM24 A+B+C at every sample', not mismatch.any(), mismatchedSamples=int(mismatch.sum()))
        ref = np.memmap(folder/'capture.f32', '<f4', mode='r').reshape(-1, 8)
        alignment = align_exact(ref[:, :2], audio['A'], take['startFrameLower'], take.get('startFrameUpper', take['startFrameLower'])+participant['block'], rate)
        check('A sample clock', alignment['ok'], **{k:v for k,v in alignment.items() if k!='ok'})
        if alignment.get('startFrame') is not None:
            first = alignment['startFrame']
            from audio import quantize24
            for role,ch in (('B',4),('C',6)):
                if role in audio:
                    a = audio[role]; b = quantize24(ref[first:first+len(a),ch:ch+2])
                    check(f'{role}: same start sample and exact OBS PCM', a.shape == b.shape and np.array_equal(a,b),
                          startFrame=first, mismatchedSamples=int(np.count_nonzero(a!=b)) if a.shape==b.shape else None)
    else:
        expected_seconds = expected_frames/participant['rate']
        check('MP3 duration within encoder delay/padding', abs(len(audio['A'])/rate-expected_seconds) < .15,
              decodedSeconds=len(audio['A'])/rate, expectedSeconds=expected_seconds)
    result['markers'] = {}
    for role, voice in expected.items():
        if role not in audio:
            continue
        pcm = audio[role].astype(float) / (8388608 if is_wav else 1)
        result['markers'][role] = marker_positions(pcm, rate, voice)
        check(f'{role}: marker present', len(result['markers'][role]) >= 2, count=len(result['markers'][role]))
        measured = amplitudes(pcm, rate)
        check(f'{role}: only expected person', measured[voice][0][0] > .04 and all(measured[v][0][0] < .0024 for v in range(4) if v!=voice), tones=measured)
        if voice == 3:
            # MP3 stereo encoders can introduce small independent errors.
            check(f'{role}: web centred', np.max(abs(pcm[:,0]-pcm[:,1])) < (2e-5 if is_wav else .02))
        mix = audio['D'].astype(float)/(8388608 if is_wav else 1)
        marks = marker_positions(mix, rate, voice)
        offsets = [min((abs(p-q) for q in marks), default=rate) for p in result['markers'][role]]
        check(f'{role}: markers aligned with D', bool(offsets) and max(offsets) <= (2 if is_wav else rate*.005),
              maximumOffsetFrames=max(offsets, default=None))
    check('Recorder error', take['snapshot']['error'] == 0, error=take['snapshot']['error'], detail=take['snapshot']['errorDetail'])
    return result
