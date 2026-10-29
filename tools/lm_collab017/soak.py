"""세 PC 합방 장시간 시험. prepare -> run -> cleanup (600초 실행은 운영자가 선택)."""
from __future__ import annotations
import argparse
import base64
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import threading
import time
import uuid
import zipfile

sys.dont_write_bytecode=True
from telemetry import HIDDEN, read_json, write_json, redact_text, process_info, stop_owned, lower_priority
from soak_worker import digest, checked, collect, remove
from soak_report import summarize, write_summary

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
STATE=HERE/'artifacts/soak-state.json'
PRODUCTION='https://xn--2j1bo5s81gjmag88b.com/api/livemix/collab/'
LOCAL_PROBE=Path('C:/Users/claude/gocue/build/vs2022/LiveMixCollabProbe_artefacts/Release/LiveMixCollabProbe.exe')
DEFAULT_PROBE=ROOT/'build/vs2022/LiveMixCollabProbe_artefacts/Release/LiveMixCollabProbe.exe'
NETWORKS={'local':'사무실 192.168.0.135 / 공인 222.251.250.72',
          'roomc':'강남 192.168.1.25 / 공인 218.153.167.133',
          'gompc':'사무실 192.168.0.184 / 공인 222.251.250.72 (local과 같은 공유기)'}
PCS={'local':{'root':str(ROOT),'python':sys.executable,'channel':None},
     'roomc':{'root':'C:/Users/dance/lm_soak','python':'python','channel':'chrome'},
     'gompc':{'root':'C:/Users/doomin/lm_soak','python':'C:/Users/doomin/lm_soak/venv/Scripts/python.exe','channel':'chrome'}}
CASES={
    'W3-roomc':{'host':'roomc','native':['local'],'web':'gompc','record':'wav'},
    'W3-local44':{'host':'local','native':['gompc'],'web':'roomc','wireRate':44100},
    'N3':{'host':'local','native':['roomc','gompc']},
    'W2-relay':{'host':'roomc','web':'local','forceRelay':True},
    'W3-churn':{'host':'roomc','native':['gompc'],'web':'local','churnAt':240,'churnGap':60},
    'RTT-native':{'host':'gompc','native':['roomc'],'rtt':True},
    'RTT-web':{'host':'local','web':'roomc','rtt':True},
}
DEFAULT_CASES='W3-roomc,W3-local44,N3,W2-relay+RTT-native,W3-churn+RTT-web'


def module(path, name):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value);return value


def quote(value):return "'"+str(value).replace("'","''")+"'"


def powershell(script):
    prefix="""$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
$OutputEncoding = [System.Text.UTF8Encoding]::new()
[System.Diagnostics.Process]::GetCurrentProcess().PriorityClass = 'BelowNormal'
$env:PYTHONUTF8 = '1'
$env:PYTHONDONTWRITEBYTECODE = '1'
$ProgressPreference = 'SilentlyContinue'
$env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User')
"""
    encoded=base64.b64encode((prefix+script).encode('utf-16-le')).decode('ascii')
    return 'powershell -NoProfile -NonInteractive -EncodedCommand '+encoded


class Remote:
    def __init__(self,name):
        self.name=name;self.config=PCS[name];self.root=self.config['root'];self.python=self.config['python']
        if name=='roomc':
            helper=module('C:/Users/claude/tools/build_server/rbuild.py','soak_rbuild')
            self.ssh=helper.SSH[:];self.scp=helper.SCP[:];self.where=f'{helper.USER}@{helper.HOST}'
        else:
            helper=module('C:/Users/claude/tools/hanime_scratch/upscale_cmp/gssh.py','soak_gssh')
            options=['-i',helper.KEY,'-o','BatchMode=yes','-o','ConnectTimeout=8',
                     '-o','UserKnownHostsFile=C:/Users/claude/.ssh/known_hosts','-o','LogLevel=ERROR']
            self.where=f'{helper.USER}@{helper.host()}'
            self.ssh=[helper.SSH,*options,self.where];self.scp=[helper.SCP,'-q',*options]
        self.worker=self.root+'/soak-kit/tools/lm_collab017/soak_worker.py'

    def command(self,script):return self.ssh+[powershell(script)]

    def ps(self,script,timeout=60):
        r=subprocess.run(self.command(script),capture_output=True,encoding='utf-8',errors='replace',timeout=timeout,creationflags=HIDDEN)
        if r.returncode:raise RuntimeError(f'{self.name}: SSH {r.returncode}: '+redact_text(r.stderr+r.stdout)[-1500:])
        return r.stdout.strip()

    def request(self,request,timeout=60):
        script=f'& {quote(self.python)} -X utf8 {quote(self.worker)}\nexit $LASTEXITCODE'
        r=subprocess.run(self.command(script),input=json.dumps({'root':self.root,**request}),capture_output=True,
                         encoding='utf-8',errors='replace',timeout=timeout,creationflags=HIDDEN)
        try:value=json.loads(r.stdout.strip())
        except ValueError:raise RuntimeError(f'{self.name}: invalid worker response: '+redact_text(r.stderr+r.stdout)[-1500:])
        if r.returncode or 'error' in value:raise RuntimeError(f'{self.name}: '+redact_text(str(value)))
        return value

    def copy(self,source,target,download=False):
        source_remote=str(source).replace('\\','/');target_remote=str(target).replace('\\','/')
        args=[f'{self.where}:{source_remote}',str(target)] if download else [str(source),f'{self.where}:{target_remote}']
        r=subprocess.run(self.scp+args,capture_output=True,encoding='utf-8',errors='replace',timeout=180,creationflags=HIDDEN)
        if r.returncode:raise RuntimeError(f'{self.name}: SCP failed: '+redact_text(r.stderr)[-1200:])

    def write(self,path,text):return self.request({'action':'write','path':path,'text':text})
    def read(self,path):return self.request({'action':'read','path':path})['text']


def payload():
    names=('endpoint.py','run.py','audio.py','browser.py','browser.mjs','observer.js','rtc_observer.js',
           'recordings.py','entry.html','telemetry.py','soak_worker.py')
    files={f'soak-kit/tools/lm_collab017/{name}':HERE/name for name in names}
    for name in ('run_http.py','run_browser.py'):files['soak-kit/tests/livemix017/r6/'+name]=ROOT/'tests/livemix017/r6'/name
    for p in (ROOT/'livemix/webjoin').rglob('*.js'):
        files['soak-kit/'+p.relative_to(ROOT).as_posix()]=p
    return files


def install_local(probe):
    before=digest(LOCAL_PROBE);wanted=digest(probe)
    if not wanted:raise FileNotFoundError(probe)
    backup=LOCAL_PROBE.with_name('LiveMixCollabProbe.exe.bak_0175')
    if not backup.exists():
        if before is None:raise FileNotFoundError('firewall-allowed original probe is missing')
        shutil.copy2(LOCAL_PROBE,backup)
    if before!=wanted:shutil.copy2(probe,LOCAL_PROBE)
    if digest(LOCAL_PROBE)!=wanted:raise RuntimeError('local probe hash mismatch')
    return {'probe':str(LOCAL_PROBE),'probeSha256':wanted,'backupSha256':digest(backup),'copied':before!=wanted}


def prepare_one(name,probe):
    if name=='local':
        result=install_local(probe)
        from soak_worker import main as worker
        result.update(worker({'action':'check','root':str(HERE/'artifacts'),'probe':str(LOCAL_PROBE)}))
        return result
    remote=Remote(name);files=payload();files['probe/LiveMixCollabProbe.exe']=probe
    # Bootstrap only the two dependency-free helper files; compare actual hashes.
    boot=['soak-kit/tools/lm_collab017/soak_worker.py','soak-kit/tools/lm_collab017/telemetry.py']
    remote.ps(f'New-Item -ItemType Directory -Force {quote(remote.root+"/soak-kit/tools/lm_collab017")} | Out-Null')
    copied=[]
    for rel in boot:
        target=remote.root+'/'+rel
        actual=remote.ps(f'if (Test-Path -LiteralPath {quote(target)}) {{ (Get-FileHash -LiteralPath {quote(target)} -Algorithm SHA256).Hash }}')
        if actual.lower()!=digest(files[rel]):remote.copy(files[rel],target);copied.append(rel)
    hashes={rel:digest(path) for rel,path in files.items()}
    actual=remote.request({'action':'manifest','files':list(files)})
    changed=[rel for rel,value in hashes.items() if actual.get(rel)!=value]
    if changed:
        archive=HERE/'artifacts'/f'payload-{name}.zip'
        with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED) as z:
            for rel in changed:z.write(files[rel],rel)
        remote.copy(archive,remote.root+'/payload.zip')
        remote.request({'action':'install','archive':remote.root+'/payload.zip'});archive.unlink()
        copied+=changed
    actual=remote.request({'action':'manifest','files':list(files)})
    if actual!=hashes:raise RuntimeError(f'{name}: payload hash mismatch')
    fixed=remote.root+'/probe/LiveMixCollabProbe.exe'
    firewall_path=fixed.replace('/','\\')
    rules=remote.ps(f"""
$rule = Get-NetFirewallRule -Group 'LiveMixSoak' -ErrorAction SilentlyContinue | Where-Object {{ $_.DisplayName -eq 'LiveMixSoak probe' }}
if (-not $rule) {{ New-NetFirewallRule -DisplayName 'LiveMixSoak probe' -Group 'LiveMixSoak' -Direction Inbound -Action Allow -Program {quote(firewall_path)} -Profile Any | Out-Null }}
$allowed = @(Get-NetFirewallRule -Group 'LiveMixSoak' -Enabled True -Direction Inbound -Action Allow | Get-NetFirewallApplicationFilter | Where-Object {{ $_.Program.Replace('\\','/') -eq {quote(fixed)} }})
$allowed.Count
""")
    if int(rules or 0)<1:raise RuntimeError('fixed probe firewall rule not verified')
    result=remote.request({'action':'check','channel':PCS[name]['channel'],'probe':fixed},timeout=60)
    result.update(probe=fixed,files=len(files),copiedFiles=len(copied),copied=copied,hashes=hashes,firewallGroup='LiveMixSoak')
    return result


def prepare(args):
    HERE.joinpath('artifacts').mkdir(exist_ok=True)
    state=read_json(STATE);state.setdefault('pcs',{})
    state.setdefault('runs',[]);state['sourceProbe']=str(args.probe.resolve())
    errors={}
    # Mutation/installation per PC is sequential, so cleanup state is durable
    # immediately after each PC and a failure cannot lose successful prepares.
    for name in args.pcs.split(','):
        print(f'PREPARE {name}',flush=True)
        try:
            state['pcs'][name]=prepare_one(name,args.probe.resolve())
            print(f'PREPARED {name} copied={state["pcs"][name].get("copiedFiles",state["pcs"][name].get("copied"))}',flush=True)
        except Exception as e:
            errors[name]=redact_text(f'{type(e).__name__}: {e}');print(f'PREPARE_FAILED {name}: {errors[name]}',flush=True)
        write_json(STATE,state)
    write_json(HERE/'artifacts/prepare.json',{'ok':not errors,'requestedPCs':args.pcs.split(','),
               'allThreePrepared':set(PCS)<=state['pcs'].keys(),'pcs':state['pcs'],'errors':errors})
    return 1 if errors else 0


def specs(case):
    result=[{'role':'host','pc':case['host'],'voice':0}]
    result += [{'role':f'native{i}','pc':pc,'voice':i} for i,pc in enumerate(case.get('native',[]),1)]
    if case.get('web'):result.append({'role':'web','pc':case['web'],'voice':3})
    return result


class Endpoint:
    def __init__(self,case_name,spec,case,args,run_id,code='',invite=None):
        self.spec=spec;self.name=f'{spec["role"]}@{spec["pc"]}';self.remote=None
        self.local=args.output/case_name/self.name
        self.local.parent.mkdir(parents=True,exist_ok=True)
        self.control=args.output/case_name/('control-'+self.name)
        self.control.mkdir()
        self.code=None;self.invite_file=None;self.markers=set();self.pid=None
        if spec['pc']=='local':
            self.output=str(self.local);self.control_path=str(self.control);kit=str(ROOT)
            executable=sys.executable;probe=str(LOCAL_PROBE)
        else:
            self.remote=Remote(spec['pc']);kit=self.remote.root+'/soak-kit';executable=self.remote.python
            self.output=self.remote.root+f'/soak-runs/{run_id}/{case_name}/{self.name}'
            self.control_path=self.remote.root+f'/soak-runs/{run_id}/{case_name}/control-{self.name}'
            probe=self.remote.root+'/probe/LiveMixCollabProbe.exe'
            self.remote.write(self.control_path+'/ready.json','{}')
        role='native' if spec['role'].startswith('native') else spec['role']
        command=[executable,'-u','-X','utf8',kit+'/tools/lm_collab017/endpoint.py',role,'--output',self.output,
                 '--probe',probe,'--server',args.server,'--people',str(len(specs(case))),
                 '--seconds',str(args.seconds),'--wait',str(args.wait),'--wire-rate',str(case.get('wireRate',48000)),
                 '--control-dir',self.control_path]
        if role=='host':
            if case.get('web'):command+=['--web']
            if case.get('record'):command+=['--record',case['record']]
            if case.get('rtt'):command+=['--rtt']
            if case.get('hostNoUpnp'):command+=['--no-upnp']
        elif role=='native':command+=['--code',code,'--voice',str(spec['voice'])]
        else:
            invite_path=self.control_path+'/invite.txt';self.write_control('invite.txt',invite)
            command+=['--invite-file',invite_path]
            if PCS[spec['pc']]['channel']:command+=['--channel',PCS[spec['pc']]['channel']]
            if case.get('forceRelay'):command+=['--force-relay']
        if role!='host' and case.get('rtt'):command+=['--reflect']
        if case.get('churnAt') is not None:command+=['--churn-at',str(case['churnAt']),'--churn-gap',str(case.get('churnGap',60))]
        if self.remote:
            command=self.remote.command('Set-Location '+quote(kit)+'\n& '+' '.join(quote(a) for a in command)+'\nexit $LASTEXITCODE')
        self.log_path=args.output/case_name/(self.name+'.log')
        self.process=subprocess.Popen(command,cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,
            text=True,encoding='utf-8',errors='replace',creationflags=HIDDEN)
        self.process_record=process_info(self.process.pid)
        write_json(self.control/'launcher.json',self.process_record)
        self.thread=threading.Thread(target=self.read_output,daemon=True);self.thread.start()

    def read_output(self):
        with self.log_path.open('w',encoding='utf-8') as log:
            for line in self.process.stdout:
                value=line.strip()
                if value.startswith('PROBE_CODE '):self.code=value.split(' ',1)[1];value='PROBE_CODE [control]'
                elif value.startswith('INVITE_FILE '):self.invite_file=value.split(' ',1)[1]
                elif value.startswith('ENDPOINT_PID '):self.pid=int(value.split(' ',1)[1])
                elif value in ('READY','STARTED','MEASURED'):self.markers.add(value)
                log.write(redact_text(value)+'\n');log.flush()

    def write_control(self,name,value='{}'):
        if self.remote:self.remote.write(self.control_path+'/'+name,value)
        else:(self.control/name).write_text(value,encoding='utf-8')

    def invite(self):
        if self.remote:return self.remote.read(self.invite_file).strip()
        return Path(self.invite_file).read_text(encoding='utf-8-sig').strip()

    def fetch(self,keep_audio):
        if self.remote:
            collected=self.remote.request({'action':'collect','path':self.output,'keepAudio':True},timeout=90)
            self.local.mkdir(exist_ok=True)
            archive=self.local/'reports.zip';self.remote.copy(collected['archive'],archive,download=True)
        else:
            collected=collect(self.local,keep_audio);archive=self.local/'reports.zip'
        with zipfile.ZipFile(archive) as z:
            for name in z.namelist():checked(self.local/name,self.local)
            z.extractall(self.local)
        archive.unlink()
        if self.remote:
            collected.update(self.remote.request({'action':'prune','path':self.output,'keepAudio':keep_audio},timeout=60))
        shutil.copy2(self.log_path,self.local/'endpoint.log')
        write_json(self.local/'collection.json',collected)
        return collected

    def stop(self):
        if self.remote:
            # The remote endpoint's PID file owns all eligible targets.
            try:
                records=json.loads(self.remote.read(self.output+'/owned.json'))['processes']
                self.remote.request({'action':'stop','processes':records},timeout=40)
            except Exception:
                # An unverified/recycled PID is never a fallback kill target.
                raise RuntimeError(f'{self.name}: could not read/stop owned PID records')
        elif self.local.exists():
            stop_owned(read_json(self.local/'owned.json').get('processes',[]))
        if self.process.poll() is None:stop_owned([self.process_record])
        self.process.wait(timeout=15);self.thread.join(5)


def wait_until(condition,endpoints,timeout,label):
    deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
        if condition():return
        for endpoint in endpoints:
            if endpoint.process.poll() is not None:
                raise RuntimeError(f'{endpoint.name} exited {endpoint.process.returncode} before {label}; see endpoint.log')
        time.sleep(.15)
    raise TimeoutError(label)


def together(items,fn):
    with ThreadPoolExecutor(max_workers=max(1,len(items))) as pool:
        results=list(pool.map(fn,items))
    return results


def run_case(name,case,args,run_id):
    print(f'CASE {name} starting',flush=True)
    participants=[];result={'name':name,'configuration':case,'status':'failed','participants':[]}
    try:
        roles=specs(case);host=Endpoint(name,roles[0],case,args,run_id);participants.append(host)
        wait_until(lambda:host.code,participants,50,'host room code')
        # Start the native immediately, while the host creates its web invitation.
        for role in roles[1:]:
            if role['role']!='web':participants.append(Endpoint(name,role,case,args,run_id,code=host.code))
        if case.get('web'):
            wait_until(lambda:host.invite_file,participants,35,'host invite file')
            invite=host.invite()
            participants.append(Endpoint(name,roles[-1],case,args,run_id,invite=invite));invite=None
        wait_until(lambda:all('READY' in p.markers for p in participants),participants,args.wait,'all ready')
        together(participants,lambda p:p.write_control('start.json'))
        wait_until(lambda:all('STARTED' in p.markers for p in participants),participants,30,'measurement start')
        print(f'CASE {name} measuring {args.seconds}s',flush=True)
        wait_until(lambda:all('MEASURED' in p.markers for p in participants),participants,args.seconds+35,'measurement end')
        together(participants,lambda p:p.write_control('release.json'))
        deadline=time.monotonic()+max(180,args.seconds)
        while any(p.process.poll() is None for p in participants) and time.monotonic()<deadline:time.sleep(.25)
        if any(p.process.poll() is None for p in participants):raise TimeoutError('analysis/endpoint exit')
    except Exception as error:
        result['error']=redact_text(f'{type(error).__name__}: {error}')
    finally:
        # Release connected peers even when a different role failed.
        for participant in participants:
            try:
                if participant.process.poll() is None:
                    if result.get('error'):participant.stop()
                    else:participant.process.wait(timeout=10)
                participant.thread.join(5)
                collected=participant.fetch(args.keep_audio)
                row=summarize(participant.local,participant.spec,NETWORKS[participant.spec['pc']])
                row['collection']=collected
                if (row['disconnects'] or row['reconnects']) and case.get('churnAt') is None:row['status']='failed'
                if participant.spec['role']=='web' and case.get('forceRelay'):
                    row['relayVerified']=bool(row['paths']) and all(p['route']=='relay' for p in row['paths'])
                    if not row['relayVerified']:row['status']='failed'
                if collected['processCleanup']['remaining'] or (not args.keep_audio and collected['audioRemaining']):row['status']='failed'
                result['participants'].append(row)
            except Exception as error:
                result.setdefault('collectionErrors',[]).append(f'{participant.name}: '+redact_text(str(error)))
            finally:
                # Invitation copies are disposable, including startup/timeout failures.
                try:
                    if participant.remote:participant.remote.request({'action':'delete-file','path':participant.control_path+'/invite.txt'})
                except Exception as error:result.setdefault('collectionErrors',[]).append(f'{participant.name}: invite cleanup: '+redact_text(str(error)))
                (participant.control/'invite.txt').unlink(missing_ok=True)
        result['status']='passed' if (not result.get('error') and not result.get('collectionErrors') and
            len(result['participants'])==len(specs(case)) and all(p['status']=='passed' for p in result['participants'])) else 'failed'
        write_json(args.output/name/'case.json',result)
    print(f'CASE {name} {result["status"]}',flush=True)
    return result


def load_cases(args):
    cases=dict(CASES)
    if args.case_file:
        extra=json.loads(args.case_file.read_text(encoding='utf-8-sig'))
        cases.update(extra.get('cases',extra))
    waves=[wave.split('+') for wave in args.cases.split(',')]
    names=[name for wave in waves for name in wave]
    if len(set(names))!=len(names):raise ValueError('case names must be unique in one output')
    import re
    for name in names:
        if not re.fullmatch(r'[A-Za-z0-9_-]+',name) or name not in cases:raise ValueError('unknown/invalid case '+name)
        case=cases[name];people=specs(case)
        if not 2<=len(people)<=3 or any(p['pc'] not in PCS for p in people):raise ValueError('invalid room size or PC')
        if case.get('wireRate',48000) not in (44100,48000):raise ValueError('invalid wire rate')
        if case.get('churnAt') is not None and not 0<case['churnAt']<case['churnAt']+case.get('churnGap',60)<args.seconds:
            raise ValueError(name+': seconds must include leave and rejoin')
    return cases,waves


def run(args):
    if args.seconds<3 or args.wait+args.seconds+30>1800:raise ValueError('duration outside probe limit')
    cases,waves=load_cases(args);state=read_json(STATE)
    needed={p['pc'] for wave in waves for name in wave for p in specs(cases[name])}
    if not needed<=state.get('pcs',{}).keys():raise RuntimeError('prepare required for '+','.join(sorted(needed-set(state.get('pcs',{})))))
    args.output=args.output.resolve();args.output.mkdir(parents=True,exist_ok=False)
    run_id=time.strftime('%Y%m%d-%H%M%S-')+uuid.uuid4().hex[:8]
    state.setdefault('runs',[]).append({'id':run_id,'output':str(args.output),'pcs':sorted(needed)})
    write_json(STATE,state)
    results=[];write_summary(args.output,results,args.seconds,waves)
    for wave in waves:
        # Each case runs independently, with its own room, controls and OBS rings.
        with ThreadPoolExecutor(max_workers=len(wave)) as pool:
            pending={pool.submit(run_case,name,cases[name],args,run_id):name for name in wave}
            for future in as_completed(pending):
                try:results.append(future.result())
                except Exception as error:results.append({'name':pending[future],'status':'failed','error':redact_text(str(error))})
                write_summary(args.output,results,args.seconds,waves)
    summary=write_summary(args.output,results,args.seconds,waves)
    print(str(args.output/'summary.md'),flush=True)
    return 0 if summary['ok'] else 1


def restore():
    backup=LOCAL_PROBE.with_name('LiveMixCollabProbe.exe.bak_0175')
    if not backup.exists():return {'restored':False,'reason':'no backup'}
    wanted=digest(backup)
    if digest(LOCAL_PROBE)!=wanted:shutil.copy2(backup,LOCAL_PROBE)
    return {'restored':digest(LOCAL_PROBE)==wanted,'sha256':wanted,'backupPreserved':True}


def cleanup(args):
    state=read_json(STATE);result={'pcs':{},'errors':{}}
    # Include partial prepares so a failed package/browser check cannot strand rules.
    for name in args.pcs.split(','):
        try:
            if name=='local':
                records=[]
                for run_record in state.get('runs',[]):
                    output=Path(run_record['output'])
                    for path in output.rglob('owned.json'):
                        # Remote PID numbers must never be killed on this PC.
                        if any(part.endswith('@local') for part in path.parts):records+=read_json(path).get('processes',[])
                    for path in output.rglob('launcher.json'):
                        info=read_json(path)
                        if info:records.append(info)
                    for path in output.rglob('invite.txt'):path.unlink(missing_ok=True)
                stopped=stop_owned(records)
                result['pcs'][name]={'processCleanup':stopped,'probe':restore()}
                if stopped['remaining']:raise RuntimeError('owned local processes remain')
                preflight=HERE/'artifacts/soak-preflight'
                if preflight.exists():remove(preflight,HERE/'artifacts')
            else:
                remote=Remote(name)
                # Worker can remove its own kit after it is loaded into memory.
                exists=remote.ps(f'Test-Path -LiteralPath {quote(remote.worker)}').strip()=='True'
                cleaned=remote.request({'action':'cleanup','removeProbe':True},timeout=90) if exists else {'ok':True,'workerAbsent':True}
                rules=remote.ps("Get-NetFirewallRule -Group 'LiveMixSoak' -ErrorAction SilentlyContinue | Remove-NetFirewallRule\n@(Get-NetFirewallRule -Group 'LiveMixSoak' -ErrorAction SilentlyContinue).Count")
                result['pcs'][name]={**cleaned,'firewallRulesRemaining':int(rules or 0)}
                if not cleaned['ok'] or int(rules or 0):raise RuntimeError('cleanup incomplete')
            state.get('pcs',{}).pop(name,None)
        except Exception as e:result['errors'][name]=redact_text(f'{type(e).__name__}: {e}')
        write_json(STATE,state)
    write_json(HERE/'artifacts/cleanup.json',result)
    print(json.dumps(result,ensure_ascii=False),flush=True)
    return 1 if result['errors'] else 0


def main():
    lower_priority()
    p=argparse.ArgumentParser(description=__doc__);commands=p.add_subparsers(dest='action',required=True)
    prep=commands.add_parser('prepare');prep.add_argument('--probe',type=Path,default=DEFAULT_PROBE);prep.add_argument('--pcs',default='local,roomc,gompc')
    execute=commands.add_parser('run');execute.add_argument('--cases',default=DEFAULT_CASES)
    execute.add_argument('--case-file',type=Path);execute.add_argument('--seconds',type=int,default=600)
    execute.add_argument('--wait',type=int,default=180);execute.add_argument('--output',type=Path,required=True)
    execute.add_argument('--keep-audio',action='store_true');execute.add_argument('--server',default=PRODUCTION)
    clean=commands.add_parser('cleanup');clean.add_argument('--pcs',default='local,roomc,gompc')
    commands.add_parser('restore');commands.add_parser('cases')
    args=p.parse_args()
    if getattr(args,'pcs',None) and any(name not in PCS for name in args.pcs.split(',')):p.error('unknown PC')
    try:
        if args.action=='prepare':return prepare(args)
        if args.action=='run':return run(args)
        if args.action=='cleanup':return cleanup(args)
        if args.action=='restore':print(json.dumps(restore()));return 0
        print(json.dumps(CASES,ensure_ascii=False,indent=2));return 0
    except Exception as error:
        print(redact_text(f'{type(error).__name__}: {error}'),file=sys.stderr);return 2


if __name__=='__main__':sys.exit(main())
