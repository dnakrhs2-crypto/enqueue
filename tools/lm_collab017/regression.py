"""Run the unchanged transport plugin test without touching the user's profile/rings/temp files."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import uuid

ROOT=Path(__file__).resolve().parents[2]
REQUIRED=('LiveMix real transport probe','LiveMix Recorder engine integration')
COLLAB=('LiveMix Collab protocol','LiveMix Collab sender and rates','LiveMix Collab playout','LiveMix Collab clock',
        'LiveMix Collab engine','LiveMix Collab roster','LiveMix Collab room v2','LiveMix Collab Session',
        'LiveMix Collab R8 session integration','LiveMix Collab sources','LiveMix Collab UDP','LiveMix Collab v2')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tests',type=Path,default=ROOT/'build/vs2022/tests/EnqueueTests_artefacts/Release/EnqueueTests.exe')
    parser.add_argument('--fixture',type=Path,default=Path('C:/Users/claude/tools/claude_harness/transport_probe/build/TransportProbe_artefacts/Release'))
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--suite',choices=['required','collab'],default='required',help='collab: existing native Collab tests, excluding window/UI tests')
    parser.add_argument('--filter',action='append',choices=REQUIRED+COLLAB,help='repeatable selection for a failed test reproduction')
    args=parser.parse_args();args.output=args.output.resolve();args.output.mkdir(parents=True,exist_ok=False)
    labels=args.filter or (REQUIRED if args.suite=='required' else COLLAB)
    if 'LiveMix real transport probe' in labels:
        for path in ('VST/Transport Probe.dll','VST3/Transport Probe.vst3'):
            if not (args.fixture/path).exists():parser.error('transport fixture required; a skipped opt-in test is not a pass')
    env=dict(os.environ)
    for key,part in [('TEMP','temp'),('TMP','temp'),('USERPROFILE','profile'),('APPDATA','profile/roaming'),('LOCALAPPDATA','profile/local')]:
        directory=args.output/part;directory.mkdir(parents=True,exist_ok=True);env[key]=str(directory)
    base='Local\\LiveMixR9.'+uuid.uuid4().hex
    env.update(LIVEMIX_OBS_RING=base,LIVEMIX_OBS_PEER_RING=base+'.Peer',LIVEMIX_OBS_PEER2_RING=base+'.Peer2',
               LIVEMIX_ALLOW_MULTI='1',LIVEMIX_COLLAB_LOOPBACK='1',LIVEMIX_TRANSPORT_PROBE_DIR=str(args.fixture.resolve()))
    result=[]
    for label in labels:
        test_env=env.copy()
        if label=='LiveMix Collab engine':
            # This existing test owns TWO explicit base names in one process.
            # Peer overrides would incorrectly collapse its independent fixture rings.
            test_env.pop('LIVEMIX_OBS_PEER_RING');test_env.pop('LIVEMIX_OBS_PEER2_RING')
        completed=subprocess.run([str(args.tests.resolve()),label],env=test_env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,
                                 creationflags=subprocess.CREATE_NO_WINDOW,timeout=120)
        text=completed.stdout.decode('utf-8',errors='replace')
        (args.output/(label.replace(' ','-')+'.log')).write_text(text,encoding='utf-8')
        counts=re.search(r'(\d+) passed, (\d+) failed',text)
        passed,failed=(int(counts[1]),int(counts[2])) if counts else (0,0)
        result.append({'filter':label,'exit':completed.returncode,'passed':passed,'failed':failed,
                       'ok':completed.returncode==0 and 'skipped:' not in text and passed>0 and failed==0,'output':text})
    (args.output/'result.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps([{k:v for k,v in r.items() if k!='output'} for r in result],ensure_ascii=False))
    return 0 if all(r['ok'] for r in result) else 1


if __name__=='__main__':sys.exit(main())
