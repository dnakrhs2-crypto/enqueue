"""Noninteractive helper for soak.py; JSON over SSH stdin, no credentials in argv."""
from __future__ import annotations
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import shutil
import sys
import time
import zipfile

sys.dont_write_bytecode = True
from telemetry import read_json, write_json, redact, redact_text, stop_owned, process_info, Owned


def checked(path, root, allow_root=False):
    path, root = Path(path).resolve(), Path(root).resolve()
    if not path.is_relative_to(root) or (path==root and not allow_root):
        raise ValueError('path outside owned directory')
    return path


def remove(path, root):
    path=checked(path,root)
    if path.is_dir():shutil.rmtree(path)
    else:path.unlink(missing_ok=True)


def digest(path):
    path=Path(path)
    if not path.is_file():return None
    with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()


def collect(folder, keep_audio=False):
    folder=Path(folder).resolve()
    records={}
    for file in folder.rglob('owned.json'):
        for info in read_json(file).get('processes',[]):records[info['pid']]=info
    cleanup=stop_owned(list(records.values()),exclude=[os.getpid()])
    archive=folder/'reports.zip'
    with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED) as z:
        for path in folder.rglob('*'):
            rel=path.relative_to(folder)
            if not path.is_file() or 'profile' in rel.parts:continue
            if path.name in ('invite.txt','web-microphone.wav'):continue
            if path.suffix.lower() not in ('.json','.log'):continue
            content=path.read_text(encoding='utf-8-sig',errors='replace')
            if path.suffix=='.json':content=json.dumps(redact(json.loads(content)),ensure_ascii=False,indent=2)
            else:content=redact_text(content)
            z.writestr(rel.as_posix(),content)
    return {'archive':archive.as_posix(),'processCleanup':cleanup,**prune(folder,keep_audio)}


def prune(folder,keep_audio=False):
    folder=Path(folder).resolve();removed=0
    for path in folder.rglob('*'):
        if not path.is_file():continue
        if path.name=='invite.txt' or (not keep_audio and path.suffix.lower() in ('.f32','.wav','.mp3')):
            removed+=path.stat().st_size;remove(path,folder)
    large=[str(p.relative_to(folder)) for p in folder.rglob('*') if p.is_file() and p.suffix.lower() in ('.f32','.wav','.mp3')]
    return {'deletedAudioBytes':removed,'audioRemaining':large}


def main(request):
    action=request['action'];root=Path(request['root']).resolve()
    if action=='manifest':
        return {rel:digest(checked(root/rel,root)) for rel in request['files']}
    if action=='install':
        archive=checked(request['archive'],root)
        with zipfile.ZipFile(archive) as z:
            for name in z.namelist():checked(root/name,root)
            z.extractall(root)
        archive.unlink()
        return {'installed':True}
    if action=='check':
        import numpy
        from browser import BrowserPool
        folder=checked(root/'soak-preflight',root);folder.mkdir(exist_ok=True)
        owned=Owned(folder/'owned.json')
        pool=BrowserPool(folder,request.get('channel'))
        try:
            pool.launch();owned.sample();context=pool.browser.new_context();page=context.new_page();page.goto('about:blank')
            version=pool.browser.version;context.close()
        finally:
            owned.sample();pool.close()
            cleanup=stop_owned(list(owned.items.values()),exclude=[os.getpid()])
            if not cleanup['remaining']:remove(folder,root)
        if cleanup['remaining']:raise RuntimeError('preflight child processes remain')
        return {'python':sys.version.split()[0],'executable':sys.executable,'numpy':numpy.__version__,
                'playwright':importlib.metadata.version('playwright'),'browser':version,
                'computer':os.environ.get('COMPUTERNAME'),'probeSha256':digest(request['probe']),'processCleanup':cleanup}
    if action=='read':
        path=checked(request['path'],root)
        for attempt in range(20):
            # the probe replaces invite.txt on every status pass; a read on the replace is refused
            try:return {'text':path.read_text(encoding='utf-8-sig')}
            except PermissionError:
                if attempt==19:raise
                time.sleep(.1)
    if action=='stop':
        return stop_owned(request['processes'],exclude=[os.getpid()])
    if action=='write':
        path=checked(request['path'],root);path.parent.mkdir(parents=True,exist_ok=True)
        path.write_text(request['text'],encoding='utf-8')
        return {'written':True}
    if action=='delete-file':
        path=checked(request['path'],root)
        path.unlink(missing_ok=True)
        return {'deleted':True}
    if action=='collect':
        folder=checked(request['path'],root/'soak-runs')
        return collect(folder,request.get('keepAudio',False))
    if action=='prune':
        folder=checked(request['path'],root/'soak-runs')
        return prune(folder,request.get('keepAudio',False))
    if action=='cleanup':
        runs=root/'soak-runs';records={};summaries=[]
        for file in runs.rglob('owned.json') if runs.exists() else []:
            for info in read_json(file).get('processes',[]):records[info['pid']]=info
        for info in request.get('processes',[]):records[info['pid']]=info
        cleanup=stop_owned(list(records.values()),exclude=[os.getpid()])
        if cleanup['remaining']:return {'ok':False,**cleanup}
        for name in ('soak-runs','soak-kit','soak-preflight'):
            if (root/name).exists():remove(root/name,root)
        probe=root/'probe/LiveMixCollabProbe.exe'
        if request.get('removeProbe') and probe.exists():remove(probe,root)
        return {'ok':True,**cleanup,'executionDirectoriesRemaining':(root/'soak-runs').exists()}
    raise ValueError('unknown worker action')


if __name__=='__main__':
    try:
        response=main(json.load(sys.stdin))
        print(json.dumps(response,ensure_ascii=False))
    except Exception as error:
        print(json.dumps({'error':redact_text(f'{type(error).__name__}: {error}')},ensure_ascii=False));sys.exit(1)
