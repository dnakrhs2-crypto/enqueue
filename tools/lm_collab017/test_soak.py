"""Analysis equivalence and the real chunk-upload handler; no production rooms."""
import functools
from http.server import ThreadingHTTPServer
import json
from pathlib import Path
import tempfile
import threading
import unittest
import urllib.error
import urllib.request

import numpy as np
from audio import continuity, signal, TONES
from continuity_reference import original_continuity
from browser import Handler
from soak import CASES, load_cases
from telemetry import lower_priority, redact, write_json
from soak_worker import collect,prune


class SoakAnalysis(unittest.TestCase):
    def test_sixty_seconds_identical_including_chunk_boundary_phase_jump(self):
        measured=[]
        for rate in (44100,48000):
            for voice in (0,3):
                pcm=signal(rate,60*rate+127,voice)
                # Both silence and a level excursion straddle the 10 s boundary.
                pcm[round(rate*9.98):round(rate*10.04)]=0
                pcm[round(rate*19.99):round(rate*20.03)]*=2
                # A pure phase step precisely BETWEEN two chunks, normal level.
                a,b=30*rate,40*rate
                f=np.arange(a,b)
                pcm[a:b]=(.12*np.sin(2*np.pi*f[:,None]*np.array(TONES[voice])/rate+.8)).astype('<f4')
                reference=original_continuity(pcm,rate,voice)
                self.assertGreater(reference['events'],0)
                for windows in (1000,137):
                    actual=continuity(pcm,rate,voice,chunk_windows=windows)
                    self.assertEqual(reference,{key:actual[key] for key in reference})
                    self.assertEqual(actual['eventsPerMinute'],actual['events']*60/actual['measuredSeconds'])
                    self.assertGreater(actual['phaseJumpWindows'],0)
                    measured.append({'rate':rate,'voice':voice,'chunkWindows':windows,'identical':True,
                                     'events':actual['events'],'phaseJumps':actual['phaseJumpWindows']})
        write_json(Path(__file__).parent/'artifacts/analysis-equivalence.json',measured)

    def test_clock_drift_is_not_a_dropout_but_a_skip_and_a_silence_are(self):
        # 10/29: the headless fake microphone reaches the host about 1,576 ppm slow; its 3,700 Hz tone then advances
        # 0.366 rad more per 10 ms window than nominal, over the raw 0.35 rad limit.
        for rate in (44100,48000):
            ppm=-1576e-6;seconds=60
            f=np.arange(seconds*rate,dtype=np.float64)
            drifting=(.12*np.sin(2*np.pi*f[:,None]*np.array(TONES[3])*(1+ppm)/rate)).astype('<f4')
            clean=continuity(drifting,rate,3)
            self.assertGreater(clean['phaseJumpWindows'],clean['windows']//2)   # the raw check flags most windows
            self.assertEqual(clean['compensated']['events'],0)
            self.assertAlmostEqual(clean['compensated']['driftPpm']['median'][0],ppm*1e6,delta=5)
            # a 2.5 ms skip (one packet dropped) at 20 s and 30 ms of silence at 40 s: exactly two events
            broken=np.concatenate([drifting[:20*rate],drifting[20*rate+round(rate*.0025):]])
            broken[40*rate:40*rate+round(rate*.03)]=0
            hit=continuity(broken,rate,3)['compensated']
            self.assertEqual(hit['events'],2,hit['eventStartMs'])
            self.assertAlmostEqual(hit['eventStartMs'][0],20000,delta=20)
            self.assertAlmostEqual(hit['eventStartMs'][1],40000,delta=20)
            # a voice with no drift keeps the raw verdict
            steady=continuity(signal(rate,seconds*rate,0),rate,0)
            self.assertEqual((steady['events'],steady['compensated']['events']),(0,0))

    def test_long_microphone_file_has_no_seam(self):
        # the fake microphone file is written in 10 s pieces; pieces must join exactly like one signal() call
        with tempfile.TemporaryDirectory() as folder:
            from browser import write_microphone
            import wave
            fixture=write_microphone(Path(folder)/'web-microphone.wav',25)
            with wave.open(str(fixture),'rb') as wav:data=np.frombuffer(wav.readframes(wav.getnframes()),'<i2')
            whole=(signal(48000,48000*25,3)[:,0]*32767).round().astype('<i2')
            self.assertTrue(np.array_equal(data,whole))
            mono=np.repeat((data/32767).astype('<f4')[:,None],2,axis=1)
            self.assertEqual(continuity(mono,48000,3)['compensated']['events'],0)

    def test_600_second_capture_upload_without_600_second_run(self):
        # 600 s * 48 kHz * stereo * float32 = 230,400,000 bytes.
        with tempfile.TemporaryDirectory(dir=Path(__file__).parent/'artifacts') as directory:
            root=Path(directory)
            server=ThreadingHTTPServer(('127.0.0.1',0),functools.partial(Handler,directory=str(root)))
            server.capture=root/'capture.f32'
            thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
            try:
                total=600*48000*8;chunk=b'\x00\x00\x80\x3f'*(1024*1024//4)
                offset=0;requests=0
                while offset<total:
                    body=chunk[:min(len(chunk),total-offset)]
                    request=urllib.request.Request(f'http://127.0.0.1:{server.server_port}/capture?offset={offset}',data=body)
                    with urllib.request.urlopen(request,timeout=10) as response:self.assertEqual(response.status,200)
                    offset+=len(body);requests+=1
                self.assertEqual(server.capture.stat().st_size,total)
                with server.capture.open('rb') as f:
                    f.seek(1024*1024-4);self.assertEqual(f.read(8),b'\x00\x00\x80\x3f'*2)
                    f.seek(-4,2);self.assertEqual(f.read(4),b'\x00\x00\x80\x3f')
                with self.assertRaises(urllib.error.HTTPError) as error:
                    urllib.request.urlopen(urllib.request.Request(f'http://127.0.0.1:{server.server_port}/capture?offset=0',data=chunk),timeout=10)
                self.assertEqual(error.exception.code,409)
                write_json(Path(__file__).parent/'artifacts/capture-upload.json',
                           {'ok':True,'bytes':total,'requests':requests,'chunkBytes':len(chunk),'live600SecondTest':False})
            finally:server.shutdown();server.server_close();thread.join()

    def test_wave_and_churn_validation(self):
        import argparse
        args=argparse.Namespace(cases='W3-roomc,W2-relay+RTT-native',case_file=None,seconds=600)
        cases,waves=load_cases(args)
        self.assertEqual(waves,[['W3-roomc'],['W2-relay','RTT-native']])
        args.cases='W3-churn';args.seconds=30
        with self.assertRaises(ValueError):load_cases(args)

    def test_collect_then_prune_after_transfer(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).parent/'artifacts') as directory:
            root=Path(directory);(root/'capture.f32').write_bytes(b'\x00'*32)
            (root/'result.json').write_text('{"ok":true}',encoding='utf-8')
            packaged=collect(root,keep_audio=True)
            self.assertTrue(Path(packaged['archive']).is_file())
            self.assertTrue((root/'capture.f32').exists())
            cleaned=prune(root)
            self.assertEqual(cleaned,{'deletedAudioBytes':32,'audioRemaining':[]})
            self.assertTrue((root/'result.json').exists())

    def test_signaling_secrets_are_not_reports(self):
        value={'iceServers':[{'urls':'turn:private','credential':'secret'}],
               'hostToken':'private','message':'https://example.test/#v=2&invite=secret',
               'stats':{'local':{'candidateType':'relay','protocol':'udp','relayProtocol':'tls'}}}
        safe=redact(value);text=json.dumps(safe)
        self.assertNotIn('secret',text);self.assertNotIn('private',text)
        self.assertEqual(safe['stats'],value['stats'])


if __name__=='__main__':
    lower_priority();(Path(__file__).parent/'artifacts').mkdir(exist_ok=True)
    unittest.main()
