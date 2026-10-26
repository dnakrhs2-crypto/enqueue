"""Detector checks with audible faults: wrong sender, self echo, loss, phase jump, shift, split."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
import wave

sys.path.insert(0,str(Path(__file__).resolve().parents[3]/'tools/lm_collab017'))
import numpy as np
from audio import signal, matrix, continuity, roundtrip, align_exact, quantize24, amplitudes
from recordings import read_wav, mp3_headers, inspect_take


class Analysis(unittest.TestCase):
    def test_tone_level_tracks_frequency_drift_before_combining_windows(self):
        rate=48000
        drift=np.repeat([-3,-1.5,0,1.5,3],rate)
        phase=np.cumsum(2*np.pi*(np.array([2900,3100])+drift[:,None])/rate,axis=0)
        pcm=.12*np.sin(phase)
        measured=amplitudes(pcm,rate)
        for ch in (0,1):
            self.assertAlmostEqual(measured[2][ch][ch],.12,delta=.002)

    def test_browser_and_native_recording_observers_select_the_heard_channels(self):
        from run import phase_audio
        pcm=signal(48000,48000,0)
        with tempfile.TemporaryDirectory(prefix='r9-observers-') as folder:
            root=Path(folder)
            for name,channels in (('web',2),('host',8)):
                target=root/name;target.mkdir()
                (target/'result.json').write_text(json.dumps({'rate':48000,'channels':channels}))
                capture=pcm if name=='web' else np.concatenate((np.zeros_like(pcm),pcm,np.zeros((48000,4))),axis=1)
                capture.astype('<f4').tofile(target/'capture.f32')
                phase={'start':{name:{'frame':480}},'end':{name:{'frame':48000}}}
                meta,heard=phase_audio(root,phase,name)
                np.testing.assert_array_equal(heard,pcm[480:])
                self.assertTrue(matrix(heard,meta['rate'],[0])['ok'])
                del heard  # release the read-only Windows mapping before removing the fixture

    def test_complete_split_bundle_and_corrupt_mix(self):
        rate=48000;start=1234;length=96000
        a,b,c=[signal(rate,144000,i) for i in range(3)]
        capture=np.concatenate((a,b+c,b,c),axis=1)
        def put(path,pcm):
            unsigned=pcm.flatten().astype(np.uint32)&0xffffff
            packed=np.column_stack((unsigned&255,(unsigned>>8)&255,(unsigned>>16)&255)).astype('uint8')
            with wave.open(str(path),'wb') as f:
                f.setparams((2,3,rate,0,'NONE','not compressed'));f.writeframes(packed.tobytes())
        with tempfile.TemporaryDirectory(prefix='r9-bundle-') as folder:
            root=Path(folder);record=root/'record';record.mkdir();capture.astype('<f4').tofile(root/'capture.f32')
            tracks={role:quantize24(pcm[start:start+length]) for role,pcm in zip('ABC',(a,b,c))}
            tracks['D']=np.clip(sum(v.astype(np.int64) for v in tracks.values()),-8388608,8388607).astype(np.int32)
            for role,pcm in tracks.items():
                for part,first in enumerate(range(0,length,12500),1):
                    suffix=f'_{part}' if part>1 else ''
                    put(record/f'{role}_test{suffix}.wav',pcm[first:first+12500])
            take={'label':'split','format':'wav','directory':str(record),'snapshot':{'capturedFrames':length,'error':0,'errorDetail':''},
                  'startFrameLower':start-512,'startFrameUpper':start+512}
            meta={'rate':rate,'block':128}
            checked=inspect_take(take,meta,root,Path('unused.exe'),{'A':0,'B':1,'C':2},80000)
            self.assertTrue(checked['ok'],[c for c in checked['checks'] if not c['ok']])
            broken=tracks['D'][:12500].copy();broken[777,0]+=1;put(record/'D_test.wav',broken)
            checked=inspect_take(take,meta,root,Path('unused.exe'),{'A':0,'B':1,'C':2},80000)
            self.assertFalse(checked['ok'])
            mismatch=next(c for c in checked['checks'] if c['check'].startswith('D ='))
            self.assertEqual(mismatch['mismatchedSamples'],1)

    def test_every_person_and_each_stereo_channel(self):
        a,b,c=[signal(48000,144000,i) for i in range(3)]
        self.assertTrue(matrix(b+c,48000,[1,2])['ok'])
        self.assertFalse(matrix(a+b+c,48000,[1,2])['ok'])  # self returned
        self.assertFalse(matrix(b,48000,[1,2])['ok'])  # missing third person
        self.assertFalse(matrix((b[:,0:1]+c[:,0:1]).repeat(2,axis=1),48000,[1,2])['ok'])

    def test_loss_and_phase_are_not_hidden_by_average_level(self):
        pcm=signal(48000,144000,0)
        self.assertEqual(continuity(pcm,48000,0)['events'],0)
        lost=pcm.copy();lost[48000:52800]=0
        report=continuity(lost,48000,0)
        self.assertGreaterEqual(report['longestMs'],100)
        self.assertEqual(report['events'],1)
        shifted=pcm.copy();shifted[72000:]*=-1
        self.assertGreaterEqual(continuity(shifted,48000,0)['phaseJumpWindows'],1)

    def test_marker_roundtrip_uses_one_clock(self):
        pcm=signal(48000,240000,0);delay=3072
        heard=np.concatenate((np.zeros((delay,2),dtype='float32'),pcm[:-delay]))
        result=roundtrip(pcm,heard,48000)
        self.assertTrue(result['ok']);self.assertAlmostEqual(result['medianMs'],64,delta=.1)

    def test_alignment_finds_actual_start_and_rejects_missing_sample(self):
        pcm=signal(48000,144000,0);start=12345
        rec=quantize24(pcm[start:start+60000])
        result=align_exact(pcm,rec,start-2000,start+2000,48000)
        self.assertTrue(result['ok']);self.assertEqual(result['startFrame'],start)
        broken=np.concatenate((rec[:30000],rec[30001:],rec[-1:]))
        self.assertFalse(align_exact(pcm,broken,start-2000,start+2000,48000)['ok'])

    def test_pcm24_sign_and_complete_file(self):
        values=np.array([[-8388608,8388607],[1,-1],[0,12345]],dtype=np.int32)
        unsigned=values.flatten().astype(np.uint32)&0xffffff
        packed=np.column_stack((unsigned&255,(unsigned>>8)&255,(unsigned>>16)&255)).astype('uint8')
        with tempfile.TemporaryDirectory(prefix='r9-analysis-') as folder:
            path=Path(folder)/'a.wav'
            with wave.open(str(path),'wb') as f:
                f.setparams((2,3,48000,0,'NONE','not compressed'));f.writeframes(packed.tobytes())
            meta,decoded=read_wav(path)
            self.assertEqual(meta['frames'],3);np.testing.assert_array_equal(decoded,values)

    def test_mp3_header_checks_requested_bitrate_and_truncation(self):
        with tempfile.TemporaryDirectory(prefix='r9-analysis-') as folder:
            path=Path(folder)/'a.mp3'
            # MPEG1 layer3, 48kHz stereo, 320kbps, complete synthetic frame.
            frame=bytes([255,251,0xe4,0])+bytes(956)
            path.write_bytes(frame*2)
            meta=mp3_headers(path);self.assertEqual(meta['bitrateKbps'],320);self.assertEqual(meta['mpegFrames'],2304)
            path.write_bytes(frame[:-1])
            with self.assertRaises(ValueError):mp3_headers(path)


if __name__=='__main__':
    unittest.main()
