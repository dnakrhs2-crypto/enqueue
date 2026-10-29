"""Test-only telemetry and cleanup. Process selection uses recorded IDs, never names."""
from __future__ import annotations
import ctypes
from ctypes import wintypes as w
import json
import os
from pathlib import Path
import re
import subprocess
import time

HIDDEN = (subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS) if os.name == 'nt' else 0


def lower_priority():
    if os.name=='nt':
        kernel=ctypes.WinDLL('kernel32',use_last_error=True)
        kernel.GetCurrentProcess.restype=w.HANDLE
        kernel.SetPriorityClass.argtypes=[w.HANDLE,w.DWORD]
        if not kernel.SetPriorityClass(kernel.GetCurrentProcess(),0x4000):raise ctypes.WinError(ctypes.get_last_error())


def priority_job():
    """Keep Chromium's audio/renderer children from raising their process class.

    No termination/job kill flags: cleanup still uses only recorded PID trees.
    The job handle is deliberately not inheritable; membership is inherited.
    """
    class Limits(ctypes.Structure):
        _fields_=[('processTime',ctypes.c_int64),('jobTime',ctypes.c_int64),('flags',w.DWORD),
                  ('minWorkingSet',ctypes.c_size_t),('maxWorkingSet',ctypes.c_size_t),
                  ('activeProcesses',w.DWORD),('affinity',ctypes.c_size_t),
                  ('priorityClass',w.DWORD),('schedulingClass',w.DWORD)]
    kernel=ctypes.WinDLL('kernel32',use_last_error=True)
    kernel.CreateJobObjectW.argtypes=[ctypes.c_void_p,w.LPCWSTR]
    kernel.CreateJobObjectW.restype=w.HANDLE
    kernel.SetInformationJobObject.argtypes=[w.HANDLE,ctypes.c_int,ctypes.c_void_p,w.DWORD]
    kernel.AssignProcessToJobObject.argtypes=[w.HANDLE,w.HANDLE]
    kernel.GetCurrentProcess.restype=w.HANDLE
    kernel.CloseHandle.argtypes=[w.HANDLE]
    handle=kernel.CreateJobObjectW(None,None)
    if not handle:raise ctypes.WinError(ctypes.get_last_error())
    limits=Limits();limits.flags=0x20;limits.priorityClass=0x4000
    if not kernel.SetInformationJobObject(handle,2,ctypes.byref(limits),ctypes.sizeof(limits)) or not kernel.AssignProcessToJobObject(handle,kernel.GetCurrentProcess()):
        error=ctypes.get_last_error();kernel.CloseHandle(handle);raise ctypes.WinError(error)
    return handle


def redact(value):
    if isinstance(value, dict):
        return {k: redact(v) for k, v in value.items()
                if not re.search(r'token|credential|password|iceServers|sdp|inviteUrl|webInvite', k, re.I)}
    if isinstance(value, (list, tuple)):
        return [redact(v) for v in value]
    if isinstance(value, str):
        return redact_text(value)
    return value


def redact_text(value):
    value = re.sub(r'https?://[^\s\"\'<>]*#[^\s\"\'<>]*', '[invite removed]', value)
    value = re.sub(r'(?:turns?|stuns?):[^\s\"\'<>]+', '[ICE URL removed]', value, flags=re.I)
    # Native logs and exception strings may contain serialized signaling data.
    return '\n'.join('[signaling detail removed]' if re.search(
        r'iceServers|(?:invite|host|guest|session)Token|credential|password|candidate:', line, re.I) else line
        for line in value.split('\n'))


def write_json(path, value):
    path = Path(path)
    temp = path.with_suffix(path.suffix + '.tmp')
    temp.write_text(json.dumps(redact(value), ensure_ascii=False, indent=2, allow_nan=False), encoding='utf-8')
    temp.replace(path)


def read_json(path):
    try:
        return json.loads(Path(path).read_text(encoding='utf-8-sig'))
    except (OSError, ValueError):
        return {}


def process_info(pid):
    if os.name != 'nt':
        return None
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.restype = w.HANDLE
    kernel.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
    kernel.GetProcessTimes.argtypes = [w.HANDLE] + [ctypes.POINTER(w.FILETIME)]*4
    kernel.GetPriorityClass.argtypes = [w.HANDLE]
    kernel.GetPriorityClass.restype = w.DWORD
    kernel.CloseHandle.argtypes = [w.HANDLE]
    handle = kernel.OpenProcess(0x1000, False, int(pid))
    if not handle:
        if ctypes.get_last_error() == 87:
            return None
        raise OSError(ctypes.get_last_error(), f'Cannot inspect owned PID {pid}')
    try:
        created, ended, system, user = (w.FILETIME() for _ in range(4))
        if not kernel.GetProcessTimes(handle, ctypes.byref(created), ctypes.byref(ended), ctypes.byref(system), ctypes.byref(user)):
            raise ctypes.WinError(ctypes.get_last_error())
        ticks = lambda t: (t.dwHighDateTime << 32) | t.dwLowDateTime
        if ticks(ended):
            return None
        return {'pid': int(pid), 'created': ticks(created), 'cpuSeconds': (ticks(system)+ticks(user))/1e7,
                'priorityClass':kernel.GetPriorityClass(handle)}
    finally:
        kernel.CloseHandle(handle)


def child_ids():
    class Entry(ctypes.Structure):
        _fields_ = [('size',w.DWORD),('usage',w.DWORD),('pid',w.DWORD),('heap',ctypes.c_size_t),
                    ('module',w.DWORD),('threads',w.DWORD),('parent',w.DWORD),('priority',w.LONG),
                    ('flags',w.DWORD),('exe',w.WCHAR*260)]
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateToolhelp32Snapshot.restype = w.HANDLE
    kernel.Process32FirstW.argtypes = kernel.Process32NextW.argtypes = [w.HANDLE, ctypes.POINTER(Entry)]
    kernel.CloseHandle.argtypes = [w.HANDLE]
    handle = kernel.CreateToolhelp32Snapshot(2, 0)
    if handle == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        item = Entry(); item.size = ctypes.sizeof(item)
        more = kernel.Process32FirstW(handle, ctypes.byref(item)); rows = []
        while more:
            rows.append((int(item.pid), int(item.parent)))
            more = kernel.Process32NextW(handle, ctypes.byref(item))
        return rows
    finally:
        kernel.CloseHandle(handle)


class Owned:
    def __init__(self, path):
        self.path = Path(path)
        self.root = os.getpid()
        self.priority_job = priority_job()
        self.items = {}
        self.started = time.monotonic()
        self.sample()

    def sample(self):
        selected = {self.root}
        pairs = child_ids()
        while True:
            larger = selected | {pid for pid, parent in pairs if parent in selected}
            if larger == selected:
                break
            selected = larger
        for pid in selected:
            info = process_info(pid)
            if info:
                self.items[pid] = info
        write_json(self.path, {'root': self.root, 'priorityLimit':'BelowNormal', 'processes': list(self.items.values())})
        return {'cpuSeconds': sum(p['cpuSeconds'] for p in self.items.values()),
                'wallSeconds': time.monotonic()-self.started, 'logicalCpus': os.cpu_count(),
                'scope': 'endpoint Python + owned probe/browser descendants; one core = 100%'}


def stop_owned(records, exclude=()):
    killed, remaining = [], []
    for info in records:
        pid = info['pid']
        if pid in exclude:
            continue
        current = process_info(pid)
        if not current or current['created'] != info['created']:
            continue
        subprocess.run(['taskkill', '/PID', str(pid), '/T', '/F'], capture_output=True, creationflags=HIDDEN)
        killed.append(pid)
    for info in records:
        if info['pid'] in exclude:
            continue
        current = process_info(info['pid'])
        if current and current['created'] == info['created']:
            remaining.append(info['pid'])
    return {'killed': killed, 'remaining': remaining}


class Timeline:
    def __init__(self, folder, owned):
        self.path = folder/'timeline.json'
        self.owned = owned
        self.origin = time.monotonic()
        self.next_sample = 0
        self.last_state = None
        self.data = {'intervalSeconds': 5, 'nativeStatePollSeconds': .25,
                     'startedUnixMs': time.time()*1000, 'samples': [], 'changes': [], 'actions': []}

    def poll(self, endpoint, phase, force=False):
        elapsed = time.monotonic()-self.origin
        detailed = force or elapsed >= self.next_sample
        if endpoint is None:
            status = {'state': 'absent'}
        elif hasattr(endpoint, 'page'):
            status = endpoint.status(detailed)
        else:
            status = endpoint.status()
        session = status.get('session') or {}
        people = session.get('people', session.get('members', []))
        state = {'state': status.get('state', session.get('state')),
                 'people': [{k:p[k] for k in ('id','participantId','nick','state','streamEpoch') if k in p} for p in people]}
        for key in ('reason','message'):
            if session.get(key):state[key]=session[key]
        if state != self.last_state:
            self.data['changes'].append({'elapsedMs': elapsed*1000, 'phase': phase, **state})
            self.last_state = state
        if detailed:
            self.next_sample = elapsed + 5
            self.data['samples'].append({**status,'elapsedMs': elapsed*1000, 'phase': phase,
                                         'probeCpuModel':status.get('cpu'),'cpu':self.owned.sample()})
            write_json(self.path, self.data)
        return status

    def action(self, name, **values):
        self.data['actions'].append({'elapsedMs': (time.monotonic()-self.origin)*1000, 'action': name, **values})
        write_json(self.path, self.data)

    def close(self):
        write_json(self.path, self.data)
