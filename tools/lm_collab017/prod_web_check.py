"""LiveMix 배포 관문: 운영 방 서버로 실제 웹 참가가 되는지 확인한다(측정 도구 방장 + 헤드리스 크롬 웹 손님, 이 PC 한 대).

10/27 0.17.0: 종단 시험이 전부 로컬 시험 서버·같은 PC 직접 주소로만 돌아 '운영 서버에서는 웹 참가가 항상 실패'를
배포 뒤 gom 실사용에서야 알았다(WinINet 동시 연결 2개 제한 — 운영 서버는 NAS 중계 HTTP/1.1). 운영 경로를 그대로 쓴다:
방 서버 = 앱 기본 주소, STUN 켬, 루프백 아님. 방은 R9-H/R9-Web 이름으로 잠깐 열렸다 닫힌다.

python tools/lm_collab017/prod_web_check.py --probe <LiveMixCollabProbe.exe> [--output <새 폴더>]
종료 코드 0 = 통과(웹이 방장 소리를 끊김 없이 받고, 방장이 웹 소리를 받음), 1 = 실패, 2 = 준비 실패.
"""
import argparse
import json
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
PRODUCTION = "https://xn--2j1bo5s81gjmag88b.com/api/livemix/collab/"   # CollabRoom.cpp 기본 방 서버와 같다
NO_WINDOW_LOW = subprocess.CREATE_NO_WINDOW | 0x00004000                 # 창 없음 + 보통 아래 우선순위


def endpoint(role, output, extra, timeout=None, log=None):
    command = [sys.executable, "-X", "utf8", str(HERE / "endpoint.py"), role, "--output", str(output), *extra]
    if log is None:
        return subprocess.run(command, cwd=str(HERE.parents[1]), capture_output=True, text=True, encoding="utf-8",
                              errors="replace", creationflags=NO_WINDOW_LOW, timeout=timeout)
    return subprocess.Popen(command, cwd=str(HERE.parents[1]), stdout=log, stderr=subprocess.STDOUT, creationflags=NO_WINDOW_LOW)


def read_json(path):
    try:
        return json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--output", type=Path, help="새 폴더(없으면 임시 폴더)")
    parser.add_argument("--server", default=PRODUCTION)
    args = parser.parse_args()
    if not args.probe.is_file():
        print("WEB_CHECK prepare-failed: no probe", args.probe, flush=True)
        return 2
    root = (args.output or Path(tempfile.mkdtemp(prefix="lm-webcheck-"))).resolve()
    root.mkdir(parents=True, exist_ok=True)
    with open(root / "host.log", "w", encoding="utf-8") as log:
        host = endpoint("host", root / "host", ["--probe", str(args.probe), "--server", args.server, "--web",
                                                 "--seconds", "6", "--wait", "45"], log=log)
        invite = None
        deadline = time.time() + 60
        while time.time() < deadline and host.poll() is None and invite is None:
            for line in (root / "host.log").read_text(encoding="utf-8", errors="replace").splitlines():
                if line.startswith("INVITE_FILE "):
                    path = Path(line[len("INVITE_FILE "):].strip())
                    try:
                        text = path.read_text(encoding="utf-8-sig").strip() if path.is_file() else ""
                    except OSError:
                        # 10/29: the probe replaces this file on every status pass; a read that lands on the
                        # replace is refused by Windows (PermissionError) and the next poll reads it.
                        text = ""
                    if text:
                        invite = text
            time.sleep(0.3)
        web_rc = None
        if invite:
            # the host deletes its invite file when it ends: the guest reads its own copy
            (root / "invite").mkdir()
            (root / "invite" / "invite.txt").write_text(invite, encoding="utf-8")
            try:
                web_rc = endpoint("web", root / "web", ["--server", args.server, "--invite-file", str(root / "invite" / "invite.txt"),
                                                       "--seconds", "6", "--wait", "45"], timeout=300).returncode
            except subprocess.TimeoutExpired:
                web_rc = "timeout"
        try:
            host.wait(timeout=200)
        except subprocess.TimeoutExpired:
            subprocess.run(["taskkill", "/PID", str(host.pid), "/T", "/F"], capture_output=True)   # 이 스크립트가 띄운 번호만
    web, hosted = read_json(root / "web" / "result.json"), read_json(root / "host" / "result.json")
    # The host's continuity ends with the guest leaving first (it stops a moment earlier), so the host side is judged on
    # hearing the web voice at all; continuity is the web side's steady listening to the host.
    passed = bool(invite) and web.get("status") == "passed" and bool(hosted.get("monitor", {}).get("ok"))
    summary = dict(passed=passed, invite=bool(invite), webRc=web_rc, web=web.get("status"), webError=web.get("error"),
                   hostHeardWeb=hosted.get("monitor", {}).get("ok"), hostError=hosted.get("error"), output=str(root))
    print("WEB_CHECK " + json.dumps(summary, ensure_ascii=False), flush=True)
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
