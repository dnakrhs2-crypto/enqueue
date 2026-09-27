# -*- coding: utf-8 -*-
"""A throw-away OBS for testing the LiveMix OBS plugin without touching the real OBS on this PC.

    python portable_obs.py setup            # copy OBS once, portable mode, test profile / websocket (port 4466, no auth)
    python portable_obs.py start [--visible] # start it (hidden desktop by default) with the plugin build loaded
    python portable_obs.py stop             # end the OBS this script started (pid file)
    python portable_obs.py log              # print the newest OBS log path

The copy lives in %LOCALAPPDATA%\\LiveMixObsTest\\obs (portable_mode.txt: its config stays inside that folder).
Portable OBS 32 never reads %ProgramData%\\obs-studio\\plugins, so the plugin is loaded from the build's rundir through
OBS_PLUGINS_PATH / OBS_PLUGINS_DATA_PATH (frontend/widgets/OBSBasic.cpp AddExtraModulePaths).
"""
import ctypes, ctypes.wintypes as wt, json, os, shutil, subprocess, sys, time

# a second, independent test OBS: LMOBS_TEST_ROOT / LMOBS_TEST_PORT / LMOBS_TEST_RING (its plugin then reads that ring)
ROOT = os.environ.get("LMOBS_TEST_ROOT") or os.path.join(os.environ["LOCALAPPDATA"], "LiveMixObsTest")
OBS = os.path.join(ROOT, "obs")
BIN = os.path.join(OBS, "bin", "64bit")
CFG = os.path.join(OBS, "config", "obs-studio")
REC = os.path.join(ROOT, "rec")
PIDFILE = os.path.join(ROOT, "obs.pid")
HERE = os.path.dirname(os.path.abspath(__file__))
# LMOBS_TEST_PLUGIN_DIR: load a copied plugin (a long test must not lock the build's rundir DLL against rebuilds)
RUNDIR = os.environ.get("LMOBS_TEST_PLUGIN_DIR") or os.path.normpath(
    os.path.join(HERE, "..", "..", "obs-plugin", "build_x64", "rundir", "RelWithDebInfo"))
PORT = int(os.environ.get("LMOBS_TEST_PORT", "4466"))
# The test OBS reads a TEST ring unless told otherwise: a test must never read (or a fake writer write) the ring of a
# LiveMix that is live on this PC. LMOBS_TEST_RING=real reads the real ring (a real LiveMix end-to-end check).
_ring_env = os.environ.get("LMOBS_TEST_RING", "")
RING = "" if _ring_env.lower() == "real" else (_ring_env or "Local\\LiveMix.ObsAudio.test")


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def setup():
    if not os.path.exists(os.path.join(BIN, "obs64.exe")):
        subprocess.run(["robocopy", r"C:\Program Files\obs-studio", OBS, "/E", "/NFL", "/NDL", "/NJH", "/NP"],
                       creationflags=subprocess.CREATE_NO_WINDOW)
    write(os.path.join(OBS, "portable_mode.txt"), "")
    os.makedirs(REC, exist_ok=True)
    # ProcessPriority: OBS's own Advanced setting (many streamers pick High); LMOBS_TEST_PRIORITY overrides Normal
    write(os.path.join(CFG, "global.ini"),
          "[General]\nLastVersion=536936450\nEnableAutoUpdates=false\nMaxLogs=20\nProcessPriority="
          + os.environ.get("LMOBS_TEST_PRIORITY", "Normal") + "\n")
    write(os.path.join(CFG, "user.ini"),
          "[General]\nFirstRun=true\n\n[Basic]\nProfile=LiveMixTest\nProfileDir=LiveMixTest\n"
          "SceneCollection=LiveMixTest\nSceneCollectionFile=LiveMixTest\n")
    write(os.path.join(CFG, "basic", "profiles", "LiveMixTest", "basic.ini"),
          "[General]\nName=LiveMixTest\n\n[Output]\nMode=Advanced\n\n[AdvOut]\nRecType=Standard\n"
          # forward slashes: OBS ini values unescape backslashes ("\rec" became a carriage return + "ec")
          "RecFilePath=" + REC.replace("\\", "/") + "\nRecFormat2=mkv\nRecEncoder=obs_x264\nRecAudioEncoder=ffmpeg_flac\n"
          "RecTracks=1\nTrackIndex=1\n\n[Audio]\nSampleRate=48000\nChannelSetup=Stereo\n\n"
          "[Video]\nBaseCX=640\nBaseCY=360\nOutputCX=640\nOutputCY=360\nFPSType=0\nFPSCommon=30\n")
    write(os.path.join(CFG, "plugin_config", "obs-websocket", "config.json"),
          json.dumps({"alerts_enabled": False, "auth_required": False, "first_load": False,
                      "server_enabled": True, "server_password": "", "server_port": PORT}))
    print("setup ok:", OBS)


def start(visible=False):
    if not os.path.exists(os.path.join(RUNDIR, "livemix-obs.dll")):
        raise SystemExit("plugin not built: " + RUNDIR)
    # a killed test OBS leaves run_* in .sentinel: the next start would stop on the "unclean shutdown / safe mode"
    # question (frontend/utility/CrashHandler.cpp). Only this copy's own config is cleaned.
    sentinel = os.path.join(CFG, ".sentinel")
    if os.path.isdir(sentinel):
        for f in os.listdir(sentinel):
            if f.startswith("run_"):
                os.remove(os.path.join(sentinel, f))
    env = dict(os.environ, OBS_PLUGINS_PATH=RUNDIR, OBS_PLUGINS_DATA_PATH=RUNDIR)
    if RING:
        env["LIVEMIX_OBS_RING"] = RING
    args = [os.path.join(BIN, "obs64.exe"), "--portable", "--disable-updater", "--multi",
            "--collection", "LiveMixTest", "--profile", "LiveMixTest"]
    for attempt in (1, 2):   # the very first start of a fresh portable copy has been seen to exit at once
        if visible:
            pid = subprocess.Popen(args, cwd=BIN, env=env).pid
        else:
            pid = start_on_hidden_desktop(args, BIN, env)
        with open(PIDFILE, "w") as f:
            f.write(str(pid))
        if wait_ready(pid, 30.0):
            print("obs started pid=%d (plugin from %s)" % (pid, RUNDIR))
            return
        print("obs not ready (attempt %d)" % attempt)
    raise SystemExit("obs did not start")


def wait_ready(pid, seconds):
    """True once this OBS runs and its websocket port accepts connections."""
    import socket
    deadline = time.time() + seconds
    while time.time() < deadline:
        out = subprocess.run(["tasklist", "/FI", "PID eq %d" % pid], capture_output=True, text=True,
                             creationflags=subprocess.CREATE_NO_WINDOW).stdout
        if "obs64.exe" not in out:
            return False
        try:
            with socket.create_connection(("127.0.0.1", PORT), timeout=1.0):
                return True
        except OSError:
            time.sleep(0.5)
    return False


def start_on_hidden_desktop(args, cwd, env):
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    user32.CreateDesktopW.restype = wt.HANDLE
    name = "livemix_obs_test"
    if not user32.CreateDesktopW(name, None, None, 0, 0x10000000, None):
        raise SystemExit("CreateDesktop failed: %d" % ctypes.get_last_error())

    class STARTUPINFOW(ctypes.Structure):
        _fields_ = [("cb", wt.DWORD), ("lpReserved", wt.LPWSTR), ("lpDesktop", wt.LPWSTR), ("lpTitle", wt.LPWSTR),
                    ("dwX", wt.DWORD), ("dwY", wt.DWORD), ("dwXSize", wt.DWORD), ("dwYSize", wt.DWORD),
                    ("dwXCountChars", wt.DWORD), ("dwYCountChars", wt.DWORD), ("dwFillAttribute", wt.DWORD),
                    ("dwFlags", wt.DWORD), ("wShowWindow", wt.WORD), ("cbReserved2", wt.WORD),
                    ("lpReserved2", ctypes.POINTER(ctypes.c_byte)), ("hStdInput", wt.HANDLE),
                    ("hStdOutput", wt.HANDLE), ("hStdError", wt.HANDLE)]

    class PROCESS_INFORMATION(ctypes.Structure):
        _fields_ = [("hProcess", wt.HANDLE), ("hThread", wt.HANDLE), ("dwProcessId", wt.DWORD), ("dwThreadId", wt.DWORD)]

    si = STARTUPINFOW()
    si.cb = ctypes.sizeof(si)
    si.lpDesktop = name
    pi = PROCESS_INFORMATION()
    block = "".join("%s=%s\0" % kv for kv in env.items()) + "\0"
    cmdline = subprocess.list2cmdline(args)
    ok = kernel32.CreateProcessW(None, ctypes.create_unicode_buffer(cmdline), None, None, False,
                                 0x00000400,  # CREATE_UNICODE_ENVIRONMENT
                                 ctypes.create_unicode_buffer(block), cwd, ctypes.byref(si), ctypes.byref(pi))
    if not ok:
        raise SystemExit("CreateProcess failed: %d" % ctypes.get_last_error())
    kernel32.CloseHandle(pi.hThread)
    kernel32.CloseHandle(pi.hProcess)
    return pi.dwProcessId


def stop():
    if not os.path.exists(PIDFILE):
        print("no pid file")
        return
    pid = int(open(PIDFILE).read().strip())
    out = subprocess.run(["tasklist", "/FI", "PID eq %d" % pid], capture_output=True, text=True,
                         creationflags=subprocess.CREATE_NO_WINDOW).stdout
    if "obs64.exe" in out:   # only the OBS this script started
        subprocess.run(["taskkill", "/PID", str(pid), "/F"], capture_output=True, creationflags=subprocess.CREATE_NO_WINDOW)
        print("stopped", pid)
    else:
        print("pid %d is not our obs64.exe any more" % pid)
    os.remove(PIDFILE)


def newest_log():
    d = os.path.join(CFG, "logs")
    files = sorted((os.path.join(d, f) for f in os.listdir(d)), key=os.path.getmtime) if os.path.isdir(d) else []
    print(files[-1] if files else "no logs")


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "setup":
        setup()
    elif cmd == "start":
        start("--visible" in sys.argv)
    elif cmd == "stop":
        stop()
    elif cmd == "log":
        newest_log()
    else:
        print(__doc__)
