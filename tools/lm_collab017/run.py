"""LiveMix 0.17 R9: windowless engines, isolated OBS rings, real R6 browser and recording files."""
from __future__ import annotations

import argparse
import contextlib
import hashlib
import json
import os
from pathlib import Path
import platform
import socket
import subprocess
import sys
import time
import traceback
import uuid

import numpy as np
from audio import matrix, continuity, amplitudes, roundtrip
from browser import BrowserPool, WebGuest, ROOT
from recordings import inspect_take, read_wav, mp3_headers

sys.path.insert(0, str(ROOT / 'tests/livemix017/r6'))
from run_http import server as local_server

HIDDEN = subprocess.CREATE_NO_WINDOW if sys.platform == 'win32' else 0
VOICE_NAMES = {'R9-H':0, 'R9-N1':1, 'R9-N2':2, 'R9-Web':3}


def write_json(path, value):
    path = Path(path)
    tmp = path.with_suffix(path.suffix+'.tmp')
    tmp.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False), encoding='utf-8')
    tmp.replace(path)


def read_json(path):
    try:
        return json.loads(Path(path).read_text(encoding='utf-8-sig'))
    except (FileNotFoundError, json.JSONDecodeError, PermissionError):
        return {}


def wait_for(fn, seconds=30, description='condition'):
    end = time.monotonic()+seconds
    while time.monotonic() < end:
        result = fn()
        if result:
            return result
        time.sleep(.05)
    raise TimeoutError(description)


def port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(('0.0.0.0', 0))
        return s.getsockname()[1]


class Native:
    def __init__(self, probe, output, api, voice=0, host=False, code='', web=False, legacy=False,
                 rate=48000, wire=48000, block=128, limit=0, reflect=False, loopback=True, seconds=180, host_port=None):
        self.output, self.serial = Path(output), 0
        self.output.mkdir()
        self.result = {}
        profile = self.output/'profile'
        for name in ('AppData/Roaming', 'AppData/Local', 'Documents', 'Temp'):
            (profile/name).mkdir(parents=True, exist_ok=True)
        base = 'Local\\LiveMixR9.'+uuid.uuid4().hex
        env = dict(os.environ, LIVEMIX_ALLOW_MULTI='1', LIVEMIX_COLLAB_LOOPBACK='1' if loopback else '0',
                   LIVEMIX_COLLAB_SERVER=api.removesuffix('/api/livemix/collab/'), LIVEMIX_COLLAB_NO_UPNP='1' if loopback else '0',
                   LIVEMIX_OBS_RING=base, LIVEMIX_OBS_PEER_RING=base+'.Peer', LIVEMIX_OBS_PEER2_RING=base+'.Peer2',
                   APPDATA=str(profile/'AppData/Roaming'), LOCALAPPDATA=str(profile/'AppData/Local'),
                   USERPROFILE=str(profile), TEMP=str(profile/'Temp'), TMP=str(profile/'Temp'))
        cfg = {'output':str(self.output.resolve()), 'host':host, 'code':code, 'nick':('R9-H','R9-N1','R9-N2','R9-Web')[voice],
               'voice':voice, 'web':web, 'legacy':legacy, 'deviceRate':rate, 'wireRate':wire, 'block':block,
               'splitBytes':limit, 'reflect':reflect, 'maxSeconds':seconds, 'port':host_port or port()}
        write_json(self.output/'config.json', cfg)
        write_json(self.output/'environment.json', {k:env[k] for k in env if k.startswith('LIVEMIX_')})
        self.log = (self.output/'process.log').open('w', encoding='utf-8')
        self.process = subprocess.Popen([str(probe), '--v2', str(self.output/'config.json')], cwd=ROOT, env=env,
                                        stdout=self.log, stderr=self.log, creationflags=HIDDEN)
        write_json(self.output/'process.json', {'pid':self.process.pid, 'executable':str(probe)})

    def status(self):
        s = read_json(self.output/'status.json')
        if self.process.poll() is not None:
            raise RuntimeError(f'{self.output.name} exited {self.process.returncode}: '+(self.output/'process.log').read_text(encoding='utf-8', errors='replace')[-1600:])
        return s

    def command(self, action, **fields):
        self.serial += 1
        write_json(self.output/'command.json', {'id':self.serial, 'action':action, **fields})
        if action == 'stop':
            return
        return wait_for(lambda: (s if (s:=self.status()).get('commandId') == self.serial else None), description=f'{action} acknowledgment')

    def mark(self):
        return self.command('mark')

    def ready(self, people=None):
        def check():
            s = self.status().get('session', {})
            if s.get('state') in (3,8):
                raise RuntimeError(str(s))
            if s.get('state') != 5:
                return False
            if people is None or ('v2' in s and not s['v2']):
                return True
            return len(s.get('people', [])) == people and all(p['state']==2 for p in s['people'])
        wait_for(check, 40, f'{self.output.name}: {people} connected people')

    def record(self, label, fmt='wav', kbps=320):
        self.command('record', label=label, format=fmt, kbps=kbps)
        def check():
            rec = self.status().get('recorder', {})
            if rec.get('error'):
                raise RuntimeError(str(rec))
            return rec.get('state') == 2
        wait_for(check, 20, 'recorder starting')

    def close(self):
        try:
            if self.process.poll() is None:
                self.command('stop')
                try:
                    self.process.wait(25)
                except subprocess.TimeoutExpired:
                    self.process.kill(); self.process.wait(10)
            self.result = read_json(self.output/'result.json')
        finally:
            self.log.close()

    def crash(self):
        """Only this owned process handle; intentionally no destructors for checkpoint test."""
        self.process.kill(); self.process.wait(10); self.log.close()


def marks(participants):
    return {name:p.mark() for name,p in participants.items()}


def sample_phase(participants, phases, name, seconds):
    start = marks(participants)
    time.sleep(seconds)
    end = marks(participants)
    phases.append({'name':name, 'start':start, 'end':end})


def load_capture(folder):
    result = read_json(folder/'result.json')
    channels = result.get('channels', 2)
    pcm = np.memmap(folder/'capture.f32', '<f4', mode='r').reshape(-1, channels)
    return result, pcm


def phase_audio(output, phase, person, channel=None):
    if channel is None:
        channel = 0 if person == 'web' else 2
    meta, pcm = load_capture(output/phase.get('captureAliases',{}).get(person,person))
    a, b = phase['start'][person]['frame'], phase['end'][person]['frame']
    return meta, np.asarray(pcm[a:b, channel:channel+2])


def analyse_matrix(output, phases, voices, legacy=False):
    rows, ok = [], True
    for phase in phases:
        name = phase['name']
        if name.startswith('gain'):
            continue
        if name == 'transition':
            if len(voices)<3:
                continue
            # Only the remaining person, across the ENTIRE leave/rejoin interval.
            for listener,other in (('host','n1'),('n1','host')):
                if listener not in phase['start'] or other not in voices:
                    continue
                meta, pcm = phase_audio(output, phase, listener)
                measured = continuity(pcm, meta['rate'], voices[other])
                row = {'phase':name, 'listener':listener, 'source':other, 'continuity':measured, 'ok':measured['events']==0}
                source_nick=next(n for n,v in VOICE_NAMES.items() if v==voices[other])
                source_person=next(p for p in phase['start'][listener]['session']['people'] if p['nick']==source_nick)
                _,ring=phase_audio(output,phase,listener,2+source_person['slot']*2)
                row['ringContinuity']=continuity(ring,meta['rate'],voices[other])
                row['ok'] &= row['ringContinuity']['events']==0
                rows.append(row); ok &= row['ok']
            continue
        active = voices if name != 'absent' else {k:v for k,v in voices.items() if k!=('web' if 'web' in voices else 'n2')}
        for listener in phase['start']:
            meta, pcm = phase_audio(output, phase, listener, 0 if listener=='web' else 2)
            expected = [v for k,v in active.items() if k!=listener]
            measured = matrix(pcm, meta['rate'], expected)
            row = {'phase':name, 'listener':listener, 'monitor':measured, 'ok':measured['ok']}
            if listener != 'web':
                people = phase['start'][listener].get('session', {}).get('people', [])
                slot_voices = {p['slot']:VOICE_NAMES[p['nick']] for p in people if not p['self'] and p['nick'] in VOICE_NAMES}
                if legacy and listener=='n1':
                    slot_voices = {1:0}
                row['rings'] = []
                for slot in (1,2):
                    _, ring = phase_audio(output, phase, listener, 2+slot*2)
                    sources = [slot_voices[slot]] if slot in slot_voices else []
                    m = matrix(ring, meta['rate'], sources, mono=sources==[3])
                    row['rings'].append({'slot':slot, 'expected':sources, **m}); row['ok'] &= m['ok']
                    row['ok'] &= all(v['events']==0 for v in m['continuity'].values())
            # Stable playback must not hide bad audio behind a passing long-window spectrum.
            row['ok'] &= all(v['events']==0 for v in measured['continuity'].values())
            rows.append(row); ok &= row['ok']
    gain_rows=[]
    for phase in phases:
        if not phase['name'].startswith('gain'):
            continue
        _, listener, voice, gain = phase['name'].split(':')
        voice, gain = int(voice), float(gain)
        baseline = next(p for p in phases if p['name']=='steady')
        channel = 0 if listener=='web' else 2
        meta, full = phase_audio(output, baseline, listener, channel)
        _, changed = phase_audio(output, phase, listener, channel)
        a,b = amplitudes(full,meta['rate']), amplitudes(changed,meta['rate'])
        ratios = {str(v): b[v][0][0]/max(a[v][0][0],1e-9) for k,v in voices.items() if k!=listener}
        passed = all(abs(r-(gain if int(v)==voice else 1)) < .08 for v,r in ratios.items())
        row={'listener':listener,'voice':voice,'gain':gain,'ratios':ratios,'ok':passed}
        row['continuity']={str(v):continuity(changed,meta['rate'],v,gain if v==voice else 1)
                           for k,v in voices.items() if k!=listener and (v!=voice or gain>0)}
        row['ok'] &= all(v['events']==0 for v in row['continuity'].values())
        # Every other endpoint still hears the unmodified source; local OBS rings also stay raw.
        invariants=[]
        for other in voices:
            if other==listener or other not in phase['start']:
                continue
            meta2,x = phase_audio(output,baseline,other,0 if other=='web' else 2)
            _,y = phase_audio(output,phase,other,0 if other=='web' else 2)
            aa,bb=amplitudes(x,meta2['rate']),amplitudes(y,meta2['rate'])
            for source in voices.values():
                if source==voices[other]:
                    continue
                ratio=bb[source][0][0]/max(aa[source][0][0],1e-9)
                uninterrupted=continuity(y,meta2['rate'],source)
                invariants.append({'listener':other,'voice':source,'ratio':ratio,'continuity':uninterrupted,
                                   'ok':abs(ratio-1)<.08 and uninterrupted['events']==0})
        if listener!='web':
            for slot in (1,2):
                _,x=phase_audio(output,baseline,listener,2+slot*2)
                _,y=phase_audio(output,phase,listener,2+slot*2)
                aa,bb=amplitudes(x,meta['rate']),amplitudes(y,meta['rate'])
                for source in voices.values():
                    if aa[source][0][0]>.04:
                        ratio=bb[source][0][0]/aa[source][0][0]
                        invariants.append({'ring':slot,'voice':source,'ratio':ratio,'ok':abs(ratio-1)<.08})
        row['unchanged']=invariants;row['ok'] &= all(i['ok'] for i in invariants)
        gain_rows.append(row);ok &= row['ok']
    # Remaining occupants keep their slot numbers after a third occupant returns with a new ID.
    slot_checks=[]
    if any(p['name']=='rejoined' for p in phases):
        before=next(p for p in phases if p['name']=='steady')
        after=next(p for p in phases if p['name']=='rejoined')
        for listener in ('host','n1'):
            if listener not in before['start'] or listener not in after['start']:
                continue
            people=lambda p:{x['nick']:x for x in p['start'][listener]['session'].get('people',[]) if not x['self']}
            a,b=people(before),people(after)
            for nick,p in a.items():
                if nick not in ('R9-H','R9-N1') or nick not in b:
                    continue
                slot_checks.append({'listener':listener,'nick':nick,'before':p['slot'],'after':b[nick]['slot'],'ok':p['slot']==b[nick]['slot']})
        ok &= all(v['ok'] for v in slot_checks)
    return {'ok':bool(ok), 'rows':rows, 'gains':gain_rows, 'slotStability':slot_checks}


def run_case(args, pool, api, name, native_count, web=False, legacy=False, recording=False, rate=48000, formats=None, limit=0, rtt=False):
    output=args.output/name;output.mkdir()
    result={'name':name,'status':'failed','ok':False,'phases':[],'deviceRate':rate,'wireRate':args.wire_rate}
    participants={}; phases=result['phases']; voices={'host':0}; expected={'A':0}
    try:
        host=Native(args.probe,output/'host',api,host=True,web=web,rate=rate,wire=args.wire_rate,block=args.block,
                    limit=limit,loopback=not args.lan,seconds=max(240,args.seconds*12+120))
        participants['host']=host
        code=wait_for(lambda:host.status().get('session',{}).get('code'),40,'host room code')
        # Starting in the waiting room validates late naming and never-occupied seat deletion.
        if recording and native_count==1:
            host.record('late-seat','wav')
            time.sleep(2)
        for i in range(1,native_count+1):
            key=f'n{i}'
            participants[key]=Native(args.probe,output/key,api,voice=i,code=code,legacy=legacy,rate=rate,wire=args.wire_rate,
                                     block=args.block,reflect=rtt,loopback=not args.lan,seconds=max(240,args.seconds*12+120))
            participants[key].ready();voices[key]=i
        if web:
            invite=wait_for(lambda:(output/'host/invite.txt').read_text(encoding='utf-8-sig') if (output/'host/invite.txt').exists() else '',30,'web invite')
            participants['web']=WebGuest(pool,output/'web',api,invite,loopback=not args.lan,reflect=rtt);voices['web']=3
        for key,p in participants.items():
            if key!='web':p.ready(len(participants))
        time.sleep(3)
        people=host.status()['session']['people']
        expected.update({('B' if p['slot']==1 else 'C'):VOICE_NAMES[p['nick']] for p in people if not p['self']})
        if recording:
            for fmt,kbps in (formats or [('wav',320)]):
                label='late-seat' if native_count==1 else f'{fmt}-{rate}-{kbps}'
                if native_count!=1:host.record(label,fmt,kbps)
                sample_phase(participants,phases,label,args.seconds)
                host.command('recordStop')
            if native_count==1:
                result['expectedRename']='B_R9-N1'
        elif rtt:
            sample_phase(participants,phases,'rtt',args.seconds)
        else:
            sample_phase(participants,phases,'steady',args.seconds)
            leaver='web' if web else ('n2' if native_count==2 else 'n1')
            remaining={k:v for k,v in participants.items() if k!=leaver}
            transition={'name':'transition','start':marks(remaining)}
            if leaver=='web':
                # Preserve both browser captures; a new client receives a new participant ID.
                participants['web'].close();participants.pop('web')
                (output/'web').rename(output/'web-before')
            else:
                participants[leaver].command('leave')
            time.sleep(3)
            if len(remaining)>1:sample_phase(remaining,phases,'absent',max(3,args.seconds))
            if leaver=='web':
                participants['web']=WebGuest(pool,output/'web',api,invite,loopback=not args.lan)
            elif legacy:
                participants[leaver].close();participants.pop(leaver)
                (output/leaver).rename(output/(leaver+'-before'))
                participants[leaver]=Native(args.probe,output/leaver,api,voice=1,code=code,legacy=True,rate=rate,wire=args.wire_rate,block=args.block,loopback=not args.lan)
            else:
                participants[leaver].command('join',code=code)
            for key,p in participants.items():
                if key!='web':p.ready(len(participants))
            time.sleep(3)
            transition['end']=marks(remaining);phases.append(transition)
            sample_phase(participants,phases,'rejoined',args.seconds)
            # Resolve the original browser/legacy baseline against its preserved capture.
            for phase in phases:
                if phase['name']=='steady' and (leaver=='web' or legacy):
                    phase['captureAliases']={leaver:leaver+'-before'}
            listeners=list(participants)
            for listener in listeners:
                if legacy and listener=='n1':continue
                s=participants[listener].mark()['session']
                targets=([{'id':m['participantId'],'voice':VOICE_NAMES[m['nick']]} for m in s['members'] if m['participantId']!=s['selfId']]
                         if listener=='web' else [{'id':p['id'],'voice':VOICE_NAMES[p['nick']]} for p in s['people'] if not p['self']])
                for target in targets:
                    for gain in (.5,0.,1.):
                        if listener=='web':participants[listener].gain(target['id'],gain)
                        else:participants[listener].command('gain',person=target['id'],gain=gain)
                        time.sleep(1)
                        sample_phase(participants,phases,f'gain:{listener}:{target["voice"]}:{gain}',max(3,args.seconds/2))
        result['status']='measured'
    except Exception as error:
        result['error']=f'{type(error).__name__}: {error}';result['traceback']=traceback.format_exc()
        if pool.blocked:
            result['status']='blocked';result['blockedBySandbox']=True
    finally:
        for key,p in reversed(list(participants.items())):
            try:p.close()
            except Exception as error:result.setdefault('cleanupErrors',[]).append(f'{key}: {error}')
        result['participants']={key:read_json(output/key/'result.json') for key in participants}
        write_json(output/'run.json',result)
    completed=result['status']=='measured'
    if phases:
        try:
            if recording:
                host_result=result['participants']['host']
                result['recordings']=[inspect_take(t,host_result,output/'host',args.probe,expected,limit) for t in host_result['takes']]
                result['continuity']=[]
                for phase in phases:
                    for key in participants:
                        meta,pcm=phase_audio(output,phase,key)
                        for other,voice in voices.items():
                            if other!=key:result['continuity'].append({'listener':key,'source':other,**continuity(pcm,meta['rate'],voice)})
                result['ok']=bool(result['recordings']) and all(r['ok'] for r in result['recordings']) and all(c['events']==0 for c in result['continuity'])
                if result.get('expectedRename'):
                    result['renameOk']=all(Path(f['file']).stem==result['expectedRename'] for r in result['recordings'] for f in r['files'] if f['role']=='B')
                    result['ok'] &= result['renameOk']
            elif rtt:
                phase=phases[0];meta,pcm=load_capture(output/'host');a,b=phase['start']['host']['frame'],phase['end']['host']['frame']
                result['roundtrip']=roundtrip(pcm[a:b,:2],pcm[a:b,2:4],meta['rate']);result['ok']=result['roundtrip']['ok']
            else:
                result['matrix']=analyse_matrix(output,phases,voices,legacy);result['ok']=result['matrix']['ok']
            result['ok'] &= completed
            result['status']='passed' if result['ok'] else ('blocked' if result.get('blockedBySandbox') else 'failed')
        except Exception as error:
            result['status']='failed';result['error']=f'analysis {type(error).__name__}: {error}';result['traceback']=traceback.format_exc()
    write_json(output/'result.json',result)
    print(json.dumps({'case':name,'status':result['status'],'error':result.get('error'),'result':str(output/'result.json')},ensure_ascii=False),flush=True)
    return result


def crash_case(args, api, fmt):
    output=args.output/f'checkpoint-{fmt}';output.mkdir();p=None
    result={'name':f'checkpoint-{fmt}','ok':False,'status':'failed'}
    try:
        p=Native(args.probe,output/'host',api,host=True,wire=args.wire_rate)
        wait_for(lambda:p.status().get('session',{}).get('code'),40,'checkpoint room')
        p.record('crash',fmt)
        status=wait_for(lambda:(s if (s:=p.status()).get('recorder',{}).get('files',[{}])[0].get('checkpointFrames',0)>0 else None),15,'completed checkpoint')
        p.crash();p=None
        files=[]
        for f in status['recorder']['files']:
            path=Path(f['path']);meta=read_wav(path)[0] if fmt=='wav' else mp3_headers(path)
            readable=meta.get('frames',meta.get('mpegFrames',0))
            if fmt=='mp3':
                decoded=output/(path.stem+'.f32')
                decode=subprocess.run([str(args.probe),'--decode',str(path),str(decoded)],capture_output=True,creationflags=HIDDEN,timeout=30)
                if decode.returncode:raise RuntimeError('checkpoint MP3 decoder failed: '+decode.stderr.decode(errors='replace'))
                meta['decodedFrames']=decoded.stat().st_size//8
                if not meta['decodedFrames']:raise RuntimeError('checkpoint MP3 decoded no audio')
            files.append({'file':str(path),'checkpointFrames':f['checkpointFrames'],'readableFrames':readable,'ok':readable>=f['checkpointFrames'],**meta})
        result.update(files=files,ok=len(files)==4 and all(f['ok'] for f in files))
        result['status']='passed' if result['ok'] else 'failed'
    except Exception as error:result['error']=f'{type(error).__name__}: {error}'
    finally:
        if p:p.close()
    write_json(output/'result.json',result);return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe',type=Path,default=ROOT/'build/vs2022/LiveMixCollabProbe_artefacts/Release/LiveMixCollabProbe.exe')
    parser.add_argument('--output',type=Path,required=True,help='new disposable evidence directory; must not already exist')
    parser.add_argument('--suite',choices=['all','matrix','recordings','rtt','checkpoint'],default='all')
    parser.add_argument('--case',choices=['nnn','nnw','nw','nn','v1','native-rtt','web-rtt'])
    parser.add_argument('--seconds',type=float,default=8)
    parser.add_argument('--wire-rate',type=int,choices=[44100,48000],default=48000)
    parser.add_argument('--block',type=int,default=128)
    parser.add_argument('--channel',choices=['chrome','msedge'])
    parser.add_argument('--browser-blocked',type=Path,help='prior sandbox preflight result; skip further launches only if it records TargetClosedError')
    parser.add_argument('--server',help='R2 API base including /api/livemix/collab/; omitted = owned local server')
    parser.add_argument('--lan',action='store_true',help='enable real LAN/public candidates; no loopback forcing')
    args=parser.parse_args()
    args.probe=args.probe.resolve();args.output=args.output.resolve()
    if args.seconds<3:parser.error('--seconds must be at least 3')
    if not args.probe.is_file():parser.error('build LiveMixCollabProbe first')
    args.output.mkdir(parents=True,exist_ok=False)
    summary={'schema':2,'command':subprocess.list2cmdline([sys.executable,*sys.argv]),'machine':platform.node(),'platform':platform.platform(),
             'startedUtc':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),'cases':[]}
    with args.probe.open('rb') as binary:summary['probeSha256']=hashlib.file_digest(binary,'sha256').hexdigest()
    pool=BrowserPool(args.output,args.channel)
    if args.browser_blocked:
        preflight=read_json(args.browser_blocked)
        if not preflight.get('blockedBySandbox'):parser.error('--browser-blocked must be an actual TargetClosedError result')
        pool.blocked=preflight['error']
    service=contextlib.nullcontext(args.server) if args.server else local_server(args.output)
    try:
        with service as api:
            cases=[]
            if args.suite in ('all','matrix'):
                cases += [('nnn',2,False,False,False),('nnw',1,True,False,False),('nw',0,True,False,False),('nn',1,False,False,False),('v1',1,False,True,False)]
            if args.suite in ('all','rtt'):
                cases += [('native-rtt',1,False,False,True),('web-rtt',0,True,False,True)]
            for name,count,web,legacy,rtt in cases:
                if args.case and name!=args.case:continue
                if web and pool.blocked:
                    result={'name':name,'status':'blocked','ok':False,'blockedBySandbox':True,'error':pool.blocked}
                    (args.output/name).mkdir();write_json(args.output/name/'result.json',result)
                else:result=run_case(args,pool,api,name,count,web,legacy,rtt=rtt)
                summary['cases'].append({'name':name,'status':result['status'],'ok':result['ok'],'result':str(args.output/name/'result.json')})
                write_json(args.output/'summary.json',summary)
            if args.suite in ('all','recordings') and not args.case:
                for rate in (44100,48000,96000):
                    formats=[('wav',320)]+([('mp3',b) for b in (128,256,320)] if rate==48000 else [('mp3',320)] if rate==96000 else [])
                    result=run_case(args,pool,api,f'record-{rate}',2,recording=True,rate=rate,formats=formats)
                    summary['cases'].append({'name':result['name'],'status':result['status'],'ok':result['ok'],'result':str(args.output/result['name']/'result.json')})
                    write_json(args.output/'summary.json',summary)
                for name,count,limit in (('record-late-seat',1,0),('record-split',2,300000)):
                    result=run_case(args,pool,api,name,count,recording=True,limit=limit)
                    summary['cases'].append({'name':name,'status':result['status'],'ok':result['ok'],'result':str(args.output/name/'result.json')})
                    write_json(args.output/'summary.json',summary)
                if pool.blocked:
                    name='record-web';(args.output/name).mkdir()
                    result={'name':name,'status':'blocked','ok':False,'blockedBySandbox':True,'error':pool.blocked}
                    write_json(args.output/name/'result.json',result)
                else:
                    result=run_case(args,pool,api,'record-web',1,web=True,recording=True)
                summary['cases'].append({'name':result['name'],'status':result['status'],'ok':result['ok'],'result':str(args.output/result['name']/'result.json')})
                write_json(args.output/'summary.json',summary)
            if args.suite in ('all','checkpoint') and not args.case:
                for fmt in ('wav','mp3'):
                    result=crash_case(args,api,fmt)
                    summary['cases'].append({'name':result['name'],'status':result['status'],'ok':result['ok'],'result':str(args.output/result['name']/'result.json')})
    except Exception as error:
        summary['error']=f'{type(error).__name__}: {error}';summary['traceback']=traceback.format_exc()
    finally:
        pool.close()
        summary['ok']=bool(summary['cases']) and all(c['ok'] for c in summary['cases']) and 'error' not in summary
        summary['finishedUtc']=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime())
        write_json(args.output/'summary.json',summary)
    print(json.dumps({'ok':summary['ok'],'summary':str(args.output/'summary.json')},ensure_ascii=False))
    return 0 if summary['ok'] else 1


if __name__=='__main__':
    sys.exit(main())
