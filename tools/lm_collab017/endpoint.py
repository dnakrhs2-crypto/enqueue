"""One physical PC endpoint for R12. Run a host here and native/web guests on roomc (or conversely)."""
from __future__ import annotations
import argparse
import contextlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

from run import Native, BrowserPool, WebGuest, ROOT, HIDDEN, wait_for, write_json, marks, load_capture, VOICE_NAMES
from audio import matrix, continuity, roundtrip
from recordings import inspect_take
from telemetry import Timeline, Owned, redact, redact_text, read_json, write_json, stop_owned, lower_priority


@contextlib.contextmanager
def lan_server(output, address, port):
    with (output/'r2-server.log').open('w',encoding='utf-8') as log:
        child=subprocess.Popen([sys.executable,str(ROOT/'server/ytdown/livemix_collab.py'),'--serve',f'{address}:{port}'],
                               stdout=log,stderr=log,cwd=ROOT,creationflags=HIDDEN)
        try:
            import socket
            def check():
                if child.poll() is not None:raise RuntimeError('R2 server exited')
                try:
                    with socket.create_connection((address,port),timeout=.2):return True
                except OSError:return False
            wait_for(check,10,'LAN room server')
            yield f'http://{address}:{port}/api/livemix/collab/'
        finally:
            if child.poll() is None:child.terminate()
            try:child.wait(10)
            except subprocess.TimeoutExpired:child.kill();child.wait(5)


def parser():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('role',choices=['host','native','web'])
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--probe',type=Path,default=ROOT/'build/vs2022/LiveMixCollabProbe_artefacts/Release/LiveMixCollabProbe.exe')
    p.add_argument('--server')
    p.add_argument('--bind')
    p.add_argument('--server-port',type=int,default=8897)
    p.add_argument('--port',type=int)
    p.add_argument('--code',default='')
    p.add_argument('--voice',type=int,choices=[1,2],default=1)
    p.add_argument('--invite-file',type=Path)
    p.add_argument('--people',type=int,choices=[2,3],default=2)
    p.add_argument('--seconds',type=int,default=60)
    p.add_argument('--wait',type=int,default=180)
    p.add_argument('--web',action='store_true')
    p.add_argument('--reflect',action='store_true')
    p.add_argument('--rtt',action='store_true')
    p.add_argument('--legacy',action='store_true')
    p.add_argument('--record',choices=['wav','mp3'])
    p.add_argument('--kbps',type=int,choices=[128,256,320],default=320)
    p.add_argument('--wire-rate',type=int,choices=[44100,48000],default=48000)
    p.add_argument('--channel',choices=['chrome','msedge'])
    p.add_argument('--force-relay',action='store_true')
    p.add_argument('--no-upnp',action='store_true',help='this native/host skips UPnP (a router without automatic port opening)')
    p.add_argument('--control-dir',type=Path,help='runner sends start.json, release.json; all endpoints finish measuring before anyone leaves')
    p.add_argument('--churn-at',type=float)
    p.add_argument('--churn-gap',type=float,default=60)
    return p


def analyse(endpoint, start, end, args):
    meta,pcm=load_capture(endpoint.output)
    a,b=start['frame'],end['frame'];rate=meta['rate']
    heard=pcm[a:b,0:2] if args.role=='web' else pcm[a:b,2:4]
    result={'start':start,'end':end,'endpoint':meta,'captureDirectory':endpoint.output.name,
            'measuredSeconds':(b-a)/rate,'ok':True}
    if b<=a or b>len(pcm):
        raise ValueError('capture does not cover measurement')
    if args.rtt:
        result['roundtrip']=roundtrip(pcm[a:b,:2],heard,rate)
        result['ok']=result['roundtrip']['ok']
    elif args.reflect:
        result['scope']='reflector; audio RTT is measured on the host clock'
    else:
        s=start['session']
        if args.role=='web':sources=[VOICE_NAMES[m['nick']] for m in s['members'] if m['participantId']!=s['selfId']]
        elif args.legacy:sources=[0]
        else:sources=[VOICE_NAMES[m['nick']] for m in s['people'] if not m['self']]
        result['monitor']=matrix(heard,rate,sources)
        result['ok']=result['monitor']['ok'] and all(c['events']==0 for c in result['monitor']['continuity'].values())
        if args.churn_at is not None and args.role!='web':
            # Retain the web's deliberate silence as measured, never turn it into zero.
            result['plannedWebAbsence']={'atSeconds':args.churn_at,'gapSeconds':args.churn_gap,
                                        'webContinuityIncludesPlannedAbsence':True}
            result['remainingContinuity']={str(v):continuity(heard,rate,v) for v in sources if v!=3}
            result['ok']=result['monitor']['ok'] and all(c['events']==0 for c in result['remainingContinuity'].values())
        if args.role!='web':
            slots={m['slot']:VOICE_NAMES[m['nick']] for m in s.get('people',[]) if not m['self']}
            if args.legacy:slots={1:0}
            result['rings']=[{'slot':slot,**matrix(pcm[a:b,2+slot*2:4+slot*2],rate,[slots[slot]] if slot in slots else [],mono=slots.get(slot)==3)} for slot in (1,2)]
            result['ok'] &= all(r['ok'] and all(c['events']==0 or (args.churn_at is not None and v=='3')
                                              for v,c in r['continuity'].items()) for r in result['rings'])
        if args.record and args.role=='host':
            expected={'A':0,**{('B' if m['slot']==1 else 'C'):VOICE_NAMES[m['nick']] for m in s['people'] if not m['self']}}
            result['recordings']=[inspect_take(t,meta,endpoint.output,args.probe,expected) for t in meta['takes']]
            result['ok'] &= all(r['ok'] for r in result['recordings'])
    if args.role=='web':
        result['ok'] &= bool(meta.get('captureComplete')) and not meta.get('errors')
    else:
        result['ok'] &= meta.get('exit')==0
    return result


def main():
    lower_priority()
    p=parser();args=p.parse_args();args.output=args.output.resolve();args.probe=args.probe.resolve()
    if not args.server and not (args.role=='host' and args.bind):p.error('--server or host --bind is required')
    if args.role=='native' and not args.code:p.error('native requires --code')
    if args.role=='web' and not args.invite_file:p.error('web requires --invite-file')
    if args.seconds<3 or args.wait+args.seconds+30>1800:p.error('duration outside probe limit')
    if args.churn_at is not None and not 0<args.churn_at<args.churn_at+args.churn_gap<args.seconds:
        p.error('churn requires 0 < leave < rejoin < seconds')
    args.output.mkdir(parents=True,exist_ok=False)
    owned=Owned(args.output/'owned.json');timeline=Timeline(args.output,owned)
    print('ENDPOINT_PID '+str(os.getpid()),flush=True)
    pool=BrowserPool(args.output,args.channel,fixture_seconds=args.wait+args.seconds+120);endpoint=None;segments=[]
    result={'role':args.role,'ok':False,'status':'failed','seconds':args.seconds}
    service=lan_server(args.output,args.bind,args.server_port) if args.bind else contextlib.nullcontext(args.server)
    invite=None

    def sample(phase,force=False):
        return timeline.poll(endpoint,phase,force)

    def until(condition,timeout,description,phase='waiting'):
        deadline=time.monotonic()+timeout
        while time.monotonic()<deadline:
            status=sample(phase)
            if condition(status):return status
            time.sleep(.25)
        raise TimeoutError(description)

    def new_web(name):
        return WebGuest(pool,args.output/name,args.server,invite,loopback=False,reflect=args.reflect,force_relay=args.force_relay)

    try:
        with service as api:
            args.server=api
            if args.role=='web':
                invite=args.invite_file.read_text(encoding='utf-8-sig').strip()
                endpoint=new_web('web')
                until(lambda s:s.get('state')=='connected' and len((s.get('session') or {}).get('members',[]))>=args.people,
                      40,'web roster')
            else:
                endpoint=Native(args.probe,args.output/args.role,api,host=args.role=='host',code=args.code,
                    voice=0 if args.role=='host' else args.voice,web=args.web,legacy=args.legacy,wire=args.wire_rate,
                    loopback=False,reflect=args.reflect,host_port=args.port,seconds=args.wait+args.seconds+30,
                    no_upnp=args.no_upnp)
                owned.sample()
                if args.role=='host':
                    status=until(lambda s:s.get('session',{}).get('code'),40,'host room')
                    print('PROBE_CODE '+status['session']['code'],flush=True)
                    if args.web:
                        until(lambda s:(endpoint.output/'invite.txt').is_file(),30,'invite file')
                        print('INVITE_FILE '+str(endpoint.output/'invite.txt'),flush=True)
                until(lambda s:s.get('session',{}).get('state')==5,args.wait,'first partner')
                until(lambda s:args.legacy or (len(s.get('session',{}).get('people',[]))==args.people and
                      all(m['state']==2 for m in s['session']['people'])),40,'all people ready')
            # Keep the established three-second settling interval, with telemetry.
            settle=time.monotonic()+3
            until(lambda s:time.monotonic()>=settle,5,'settle',phase='settling')
            print('READY',flush=True)
            if args.control_dir:
                until(lambda s:(args.control_dir/'start.json').exists(),args.wait,'measurement start barrier')
            if args.record and args.role=='host':endpoint.record('soak',args.record,args.kbps)
            start=endpoint.mark();start_cpu=owned.sample();started=time.monotonic()
            timeline.action('measureStart',frame=start['frame']);sample('measuring',True)
            print('STARTED',flush=True)
            left=False;rejoined=False
            while time.monotonic()-started<args.seconds:
                elapsed=time.monotonic()-started
                if args.churn_at is not None and args.role=='web':
                    if not left and elapsed>=args.churn_at:
                        finish=endpoint.mark();timeline.action('webLeave',measurementSeconds=elapsed)
                        sample('leaving',True);endpoint.close();segments.append((endpoint,start,finish));endpoint=None;left=True
                    if left and not rejoined and elapsed>=args.churn_at+args.churn_gap:
                        timeline.action('webRejoinRequested',measurementSeconds=elapsed)
                        endpoint=new_web('web-rejoined');start=endpoint.mark();rejoined=True
                        timeline.action('webRejoined',measurementSeconds=time.monotonic()-started)
                sample('absent' if endpoint is None else 'measuring')
                time.sleep(min(.25,max(0,args.seconds-(time.monotonic()-started))))
            if endpoint is None:raise RuntimeError('web did not rejoin')
            end=endpoint.mark();end_cpu=owned.sample();sample('measured',True)
            segments.append((endpoint,start,end));timeline.action('measureEnd',frame=end['frame'])
            if args.role=='web':endpoint.finish_capture()
            if args.record and args.role=='host':endpoint.command('recordStop')
            result['cpu']={'wallSeconds':end_cpu['wallSeconds']-start_cpu['wallSeconds'],
                           'cpuSeconds':end_cpu['cpuSeconds']-start_cpu['cpuSeconds'],'logicalCpus':os.cpu_count(),
                           'scope':end_cpu['scope']}
            result['cpu']['percentOneCore']=100*result['cpu']['cpuSeconds']/result['cpu']['wallSeconds']
            result['cpu']['percentMachine']=result['cpu']['percentOneCore']/os.cpu_count()
            print('MEASURED',flush=True)
            if args.control_dir:
                until(lambda s:(args.control_dir/'release.json').exists(),25,'measurement release barrier',phase='holding')
            endpoint.close();endpoint=None
            result['segments']=[analyse(e,a,b,args) for e,a,b in segments]
            if len(result['segments'])==1:result.update(result['segments'][0])
            result['ok']=all(s['ok'] for s in result['segments'])
            result['status']='passed' if result['ok'] else 'failed'
    except Exception as error:
        result['error']=redact_text(f'{type(error).__name__}: {error}')
        if pool.blocked:result['status']='blocked';result['blockedBySandbox']=True
    finally:
        owned.sample()
        if endpoint:
            try:endpoint.close()
            except Exception as error:result.setdefault('cleanupErrors',[]).append(redact_text(str(error)))
        try:pool.close()
        except Exception as error:result.setdefault('cleanupErrors',[]).append(redact_text(str(error)))
        result['processCleanup']=stop_owned(list(owned.items.values()),exclude=[os.getpid()])
        if result['processCleanup']['remaining'] or result.get('cleanupErrors'):
            result['ok']=False;result['status']='failed'
        for path in args.output.rglob('invite.txt'):path.unlink(missing_ok=True)
        if args.invite_file:args.invite_file.unlink(missing_ok=True)
        for path in args.output.rglob('*.log'):
            path.write_text(redact_text(path.read_text(encoding='utf-8',errors='replace')),encoding='utf-8')
        timeline.close();write_json(args.output/'result.json',result)
    print('RESULT '+str(args.output/'result.json'),flush=True)
    return 0 if result['ok'] else 1


if __name__=='__main__':sys.exit(main())
