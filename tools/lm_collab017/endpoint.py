"""One physical PC endpoint for R12. Run a host here and native/web guests on roomc (or conversely)."""
from __future__ import annotations
import argparse
import contextlib
from pathlib import Path
import subprocess
import sys
import time

from run import Native, BrowserPool, WebGuest, ROOT, HIDDEN, wait_for, write_json, marks, load_capture, VOICE_NAMES
from audio import matrix, continuity, roundtrip
from recordings import inspect_take


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


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('role',choices=['host','native','web'])
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--probe',type=Path,default=ROOT/'build/vs2022/LiveMixCollabProbe_artefacts/Release/LiveMixCollabProbe.exe')
    p.add_argument('--server',help='full R2 API base; host may instead start --bind ADDRESS')
    p.add_argument('--bind',help='this PC LAN IPv4, starts an owned R2 server')
    p.add_argument('--server-port',type=int,default=8897)
    p.add_argument('--port',type=int,help='host UDP port; omitted = unused ephemeral port')
    p.add_argument('--code',default='')
    p.add_argument('--voice',type=int,choices=[1,2],default=1)
    p.add_argument('--invite-file',type=Path,help='copy the host control invite.txt into this disposable directory; never a host token')
    p.add_argument('--people',type=int,choices=[2,3],default=2)
    p.add_argument('--seconds',type=int,default=60)
    p.add_argument('--wait',type=int,default=180)
    p.add_argument('--web',action='store_true',help='host enables/accepts the one real browser')
    p.add_argument('--reflect',action='store_true',help='guest reflects heard audio for host same-clock RTT')
    p.add_argument('--rtt',action='store_true',help='host analyses marker returns from a reflecting guest')
    p.add_argument('--legacy',action='store_true')
    p.add_argument('--record',choices=['wav','mp3'])
    p.add_argument('--kbps',type=int,choices=[128,256,320],default=320)
    p.add_argument('--wire-rate',type=int,choices=[44100,48000],default=48000)
    p.add_argument('--channel',choices=['chrome','msedge'])
    args=p.parse_args();args.output=args.output.resolve();args.probe=args.probe.resolve()
    if not args.server and not (args.role=='host' and args.bind):p.error('--server or host --bind is required')
    if args.role=='native' and not args.code:p.error('native requires --code')
    if args.role=='web' and not args.invite_file:p.error('web requires --invite-file')
    if args.seconds<3 or args.wait+args.seconds+30>1800:p.error('duration outside probe limit')
    args.output.mkdir(parents=True,exist_ok=False)
    pool=BrowserPool(args.output,args.channel);endpoint=None
    result={'role':args.role,'ok':False,'status':'failed'}
    service=lan_server(args.output,args.bind,args.server_port) if args.bind else contextlib.nullcontext(args.server)
    try:
        with service as api:
            print('API '+api,flush=True)
            if args.role=='web':
                endpoint=WebGuest(pool,args.output/'web',api,args.invite_file.read_text(encoding='utf-8-sig'),loopback=False,reflect=args.reflect)
            else:
                endpoint=Native(args.probe,args.output/args.role,api,host=args.role=='host',code=args.code,
                    voice=0 if args.role=='host' else args.voice,web=args.web,legacy=args.legacy,wire=args.wire_rate,
                    loopback=False,reflect=args.reflect,host_port=args.port,seconds=args.wait+args.seconds+30)
                if args.role=='host':
                    code=wait_for(lambda:endpoint.status().get('session',{}).get('code'),40,'host room')
                    print('PROBE_CODE '+code,flush=True)
                    if args.web:print('INVITE_FILE '+str(endpoint.output/'invite.txt'),flush=True)
                wait_for(lambda:endpoint.status().get('session',{}).get('state')==5,args.wait,'partner')
                endpoint.ready(args.people)
            time.sleep(3)
            start=endpoint.mark()
            if args.record and args.role=='host':endpoint.record('lan',args.record,args.kbps)
            time.sleep(args.seconds)
            end=endpoint.mark()
            if args.record and args.role=='host':endpoint.command('recordStop')
            endpoint.close()
            meta,pcm=load_capture(endpoint.output)
            a,b=start['frame'],end['frame'];heard=pcm[a:b,0:2] if args.role=='web' else pcm[a:b,2:4]
            result.update(start=start,end=end,endpoint=meta)
            if args.rtt:
                result['roundtrip']=roundtrip(pcm[a:b,:2],heard,meta['rate']);result['ok']=result['roundtrip']['ok']
            elif args.reflect:
                result['ok']=True;result['status']='reflector-only'
            else:
                s=start['session']
                if args.role=='web':sources=[VOICE_NAMES[m['nick']] for m in s['members'] if m['participantId']!=s['selfId']]
                elif args.legacy:sources=[0]
                else:sources=[VOICE_NAMES[m['nick']] for m in s['people'] if not m['self']]
                result['monitor']=matrix(heard,meta['rate'],sources);result['ok']=result['monitor']['ok']
                result['ok'] &= all(c['events']==0 for c in result['monitor']['continuity'].values())
                if args.role!='web':
                    slots={m['slot']:VOICE_NAMES[m['nick']] for m in s.get('people',[]) if not m['self']}
                    if args.legacy:slots={1:0}
                    result['rings']=[matrix(pcm[a:b,2+slot*2:4+slot*2],meta['rate'],[slots[slot]] if slot in slots else []) for slot in (1,2)]
                    result['ok'] &= all(r['ok'] for r in result['rings'])
                if args.record and args.role=='host':
                    expected={'A':0,**{('B' if m['slot']==1 else 'C'):VOICE_NAMES[m['nick']] for m in s['people'] if not m['self']}}
                    result['recordings']=[inspect_take(t,meta,endpoint.output,args.probe,expected) for t in meta['takes']]
                    result['ok'] &= all(r['ok'] for r in result['recordings'])
            # Endpoint does not claim that the opposite PC passed or that churn/volume were tested.
            result['scope']='steady endpoint; both endpoint reports required; run.py covers leave/rejoin and volume'
            result['status']='passed' if result['ok'] else 'failed'
    except Exception as error:
        result['error']=f'{type(error).__name__}: {error}'
        if pool.blocked:result['status']='blocked';result['blockedBySandbox']=True
    finally:
        if endpoint:
            try:endpoint.close()
            except Exception:pass
        pool.close();write_json(args.output/'result.json',result)
    print(str(args.output/'result.json'),flush=True);return 0 if result['ok'] else 1


if __name__=='__main__':sys.exit(main())
