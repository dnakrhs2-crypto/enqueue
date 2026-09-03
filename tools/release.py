#!/usr/bin/env python3
"""Enqueue release helper (Windows).

Pipeline:  build (Release) -> unit tests -> Inno Setup installer -> EdDSA signature
           -> appcast.xml -> (optional) GitHub Release upload via gh -> website (gh-pages).

The website (site/) is deployed to the gh-pages branch with latest.json (installer URL, size, date) and
notes.html (every docs/release-notes/*.html, newest first).  `--site-only` redeploys it from the latest
GitHub release without building anything.

Typical use on the release machine:

    python tools/release.py --repo owner/gocue --key C:/keys/gocue_eddsa_priv.pem --publish

Environment fallbacks: GOCUE_GITHUB_REPO, GOCUE_EDDSA_PRIVATE_KEY_FILE, WINSPARKLE_DIR, ISCC, GH.
The version comes from project(Enqueue VERSION x.y.z) in CMakeLists.txt.
"""
import argparse
import datetime
import email.utils
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
from xml.sax.saxutils import escape

ROOT = pathlib.Path(__file__).resolve().parents[1]


def read_version():
    text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(r"project\(Enqueue\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)", text)
    if not match:
        sys.exit("could not find project(Enqueue VERSION x.y.z) in CMakeLists.txt")
    return match.group(1)


def first_existing(paths):
    for p in paths:
        if p and pathlib.Path(p).is_file():
            return pathlib.Path(p)
    return None


def find_iscc():
    local = os.environ.get("LOCALAPPDATA", "")
    pf86 = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    pf = os.environ.get("ProgramFiles", r"C:\Program Files")
    return first_existing([
        os.environ.get("ISCC"),
        os.path.join(local, "Programs", "Inno Setup 7", "ISCC.exe"),
        os.path.join(local, "Programs", "Inno Setup 6", "ISCC.exe"),
        os.path.join(pf86, "Inno Setup 7", "ISCC.exe"),
        os.path.join(pf86, "Inno Setup 6", "ISCC.exe"),
        os.path.join(pf, "Inno Setup 7", "ISCC.exe"),
        os.path.join(pf, "Inno Setup 6", "ISCC.exe"),
    ])


def find_winsparkle_tool(winsparkle_dir):
    base = winsparkle_dir or os.environ.get("WINSPARKLE_DIR", "")
    return first_existing([os.path.join(base, "bin", "winsparkle-tool.exe")]) if base else None


def find_gh():
    return first_existing([os.environ.get("GH"), r"C:\Program Files\GitHub CLI\gh.exe",
                           os.path.join(os.path.expanduser("~"), "tools", "gh", "bin", "gh.exe")]) or "gh"


def run(cmd, cwd=ROOT, capture=False):
    print("+", " ".join(str(c) for c in cmd), flush=True)
    result = subprocess.run([str(c) for c in cmd], cwd=str(cwd), check=True,
                            capture_output=capture, text=True)
    return result.stdout if capture else ""


def build(preset, skip_tests):
    run(["cmake", "--preset", preset])
    run(["cmake", "--build", "--preset", preset + "-release", "--", "-m", "-v:m", "-nologo"])
    if not skip_tests:
        run(["ctest", "--preset", preset + "-release"])


def make_installer(iscc, version, source_dir, output_dir):
    output_dir.mkdir(parents=True, exist_ok=True)
    run([iscc, "/Q", "/DAppVersion=" + version, "/DSourceDir=" + str(source_dir),
         "/DOutputDir=" + str(output_dir), str(ROOT / "installer" / "Enqueue.iss")])
    installer = output_dir / ("Enqueue-Setup-%s.exe" % version)
    if not installer.is_file():
        sys.exit("installer was not produced: %s" % installer)
    return installer


def sign(tool, key_file, installer):
    out = run([tool, "sign", "--verbose", "--private-key-file", key_file, str(installer)], capture=True)
    match = re.search(r'edSignature="([^"]+)"', out)
    signature = match.group(1) if match else out.strip().splitlines()[-1].strip()
    if not signature:
        sys.exit("winsparkle-tool produced no signature:\n" + out)
    return signature


def wrap_notes_for_feed(fragment):
    """The update window (WinSparkle) renders the notes in an embedded browser that takes the dialog's dark-mode
    background but keeps the default black text: give the page explicit colours so it reads in both modes."""
    return ("<html><head><meta charset=\"utf-8\"><style>"
            "body{background:#ffffff;color:#151515;font-family:'Malgun Gothic',sans-serif;font-size:13px;margin:8px 12px;}"
            "h2{font-size:17px;margin:4px 0 8px;} li{margin:4px 0;} code{background:#f0f0f0;padding:0 3px;}"
            "</style></head><body>%s</body></html>" % fragment)


def write_appcast(path, repo, version, installer, signature, notes_html):
    url = "https://github.com/%s/releases/download/v%s/%s" % (repo, version, installer.name)
    appcast_url = "https://github.com/%s/releases/latest/download/appcast.xml" % repo
    pub_date = email.utils.format_datetime(datetime.datetime.now(datetime.timezone.utc))
    length = installer.stat().st_size
    xml = """<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle" xmlns:dc="http://purl.org/dc/elements/1.1/">
  <channel>
    <title>Enqueue updates</title>
    <link>%s</link>
    <description>Enqueue release feed</description>
    <language>ko</language>
    <item>
      <title>Enqueue %s</title>
      <pubDate>%s</pubDate>
      <description><![CDATA[%s]]></description>
      <enclosure url="%s"
                 sparkle:version="%s"
                 sparkle:os="windows"
                 sparkle:installerArguments="/SILENT /SP- /NORESTART"
                 length="%d"
                 type="application/octet-stream"
                 sparkle:edSignature="%s" />
    </item>
  </channel>
</rss>
""" % (escape(appcast_url), escape(version), pub_date, notes_html, escape(url), escape(version), length, escape(signature))
    path.write_text(xml, encoding="utf-8")
    return url, appcast_url


# ---------------------------------------------------------------------------------------------------------------------
# website (GitHub Pages, branch gh-pages)

FIXED_INSTALLER_NAME = "Enqueue-Setup.exe"   # uploaded next to the versioned installer: /releases/latest/download/Enqueue-Setup.exe


def version_key(name):
    return tuple(int(p) for p in re.findall(r"\d+", name))


def read_feedback_link():
    """The beta feedback room from src/app/Links.h: the app's help menu and the website show the same link."""
    match = re.search(r'feedbackChat\s*=\s*"([^"]*)"', (ROOT / "src" / "app" / "Links.h").read_text(encoding="utf-8"))
    return match.group(1) if match else ""


def build_notes_page(site_dir):
    """notes.html: all release notes, newest first, in the site's style."""
    notes_dir = ROOT / "docs" / "release-notes"
    files = sorted(notes_dir.glob("*.html"), key=lambda p: version_key(p.stem), reverse=True)
    parts = []
    for f in files:
        fragment = f.read_text(encoding="utf-8").strip()
        parts.append('<article class="notes">%s</article>' % fragment)
    page = """<!doctype html>
<html lang="ko">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Enqueue 바뀐 점</title>
<link rel="icon" href="assets/icon.png">
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Noto+Sans+KR:wght@400;500;700&display=swap">
<link rel="stylesheet" href="style.css">
</head>
<body>
<header class="wrap top">
  <a class="brand" href="./"><img src="assets/icon.png" alt="" width="36" height="36">Enqueue</a>
  <nav class="nav"><a href="./">다운로드</a><a href="https://github.com/%s/releases">GitHub</a></nav>
</header>
<main class="wrap">
  <h1 style="font-size:28px;margin-top:12px">바뀐 점</h1>
  <p class="meta" style="margin-top:6px">버전별 변경 사항. 최신 버전이 맨 위입니다.</p>
  %s
</main>
<footer class="wrap"><div>Enqueue · 곰튀김</div></footer>
</body>
</html>
""" % (REPO_FOR_SITE, "\n  ".join(parts))
    (site_dir / "notes.html").write_text(page, encoding="utf-8")


REPO_FOR_SITE = ""


def deploy_site(repo, latest):
    """Copy site/ plus latest.json and notes.html onto the gh-pages branch and push it."""
    global REPO_FOR_SITE
    REPO_FOR_SITE = repo
    site_src = ROOT / "site"
    if not site_src.is_dir():
        sys.exit("site/ is missing")
    work = pathlib.Path(tempfile.mkdtemp(prefix="gocue-site-"))
    try:
        remote = "https://github.com/%s.git" % repo
        run(["git", "clone", "--quiet", "--branch", "gh-pages", "--depth", "1", remote, str(work / "pages")], cwd=work)
        pages = work / "pages"
        for item in pages.iterdir():
            if item.name != ".git":
                shutil.rmtree(item) if item.is_dir() else item.unlink()
        shutil.copytree(site_src, pages, dirs_exist_ok=True)
        (pages / ".nojekyll").write_text("", encoding="utf-8")
        (pages / "latest.json").write_text(json.dumps(latest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        build_notes_page(pages)
        run(["git", "add", "-A"], cwd=pages)
        status = run(["git", "status", "--porcelain"], cwd=pages, capture=True)
        if not status.strip():
            print("site      : unchanged")
            return
        run(["git", "-c", "user.name=Enqueue release", "-c", "user.email=release@gocue.invalid",
             "commit", "--quiet", "-m", "site: Enqueue %s" % latest.get("version", "")], cwd=pages)
        run(["git", "push", "--quiet", "origin", "gh-pages"], cwd=pages)
        print("site      : https://%s.github.io/%s/" % tuple(repo.split("/", 1)))
    finally:
        shutil.rmtree(work, ignore_errors=True)


def latest_from_github(gh, repo):
    """latest.json content for --site-only: read from the newest GitHub release."""
    out = run([gh, "release", "view", "--repo", repo, "--json", "tagName,publishedAt,assets"], capture=True)
    info = json.loads(out)
    version = info["tagName"].lstrip("v")
    installer = next((a for a in info["assets"] if re.match(r"(Enqueue|GoCue)-Setup-[0-9.]+\.exe$", a["name"])), None)
    if installer is None:
        sys.exit("the latest release has no Enqueue-Setup-x.y.z.exe asset")
    return {"version": version, "tag": info["tagName"], "url": installer["url"], "size": installer["size"],
            "date": info["publishedAt"], "latest_url": "https://github.com/%s/releases/latest/download/%s" % (repo, FIXED_INSTALLER_NAME),
            "feedback": read_feedback_link()}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--preset", default="local", help="CMake configure preset name (default: local)")
    parser.add_argument("--repo", default=os.environ.get("GOCUE_GITHUB_REPO", ""), help="GitHub owner/repo for release URLs")
    parser.add_argument("--key", default=os.environ.get("GOCUE_EDDSA_PRIVATE_KEY_FILE", ""), help="EdDSA private key file (winsparkle-tool generate-key)")
    parser.add_argument("--winsparkle-dir", default="", help="WinSparkle package dir (default: WINSPARKLE_DIR)")
    parser.add_argument("--notes", default="", help="release notes file (HTML or plain text)")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--skip-tests", action="store_true")
    parser.add_argument("--publish", action="store_true", help="create the GitHub release with gh and upload installer + appcast")
    parser.add_argument("--site-only", action="store_true", help="only redeploy the website from the latest GitHub release")
    parser.add_argument("--skip-site", action="store_true", help="publish without touching the website")
    args = parser.parse_args()

    if not args.repo:
        sys.exit("--repo owner/name (or GOCUE_GITHUB_REPO) is required for the appcast URLs")

    if args.site_only:
        deploy_site(args.repo, latest_from_github(find_gh(), args.repo))
        return

    # Single source of truth: the exe's VERSIONINFO, the installer name, the appcast and the
    # GitHub tag must all agree, otherwise the published appcast points at a 404.
    version = read_version()
    tag = os.environ.get("GITHUB_REF_NAME", "")
    if tag and tag != "v" + version:
        sys.exit("git tag %s does not match project(GoCue VERSION %s) - bump CMakeLists.txt or re-tag" % (tag, version))

    source_dir = ROOT / "build" / "vs2022" / "Enqueue_artefacts" / "Release"
    output_dir = ROOT / "installer" / "output"

    if not args.skip_build:
        build(args.preset, args.skip_tests)

    for required in ("Enqueue.exe", "WinSparkle.dll"):
        if not (source_dir / required).is_file():
            sys.exit("missing %s in %s (build Release with WINSPARKLE_DIR set)" % (required, source_dir))

    iscc = find_iscc()
    if iscc is None:
        sys.exit("ISCC.exe not found (install Inno Setup or set ISCC)")
    installer = make_installer(iscc, version, source_dir, output_dir)

    tool = find_winsparkle_tool(args.winsparkle_dir)
    if tool is None:
        sys.exit("winsparkle-tool.exe not found (set --winsparkle-dir or WINSPARKLE_DIR)")
    if not args.key or not pathlib.Path(args.key).is_file():
        sys.exit("EdDSA private key file not found (--key / GOCUE_EDDSA_PRIVATE_KEY_FILE)")
    signature = sign(tool, args.key, installer)

    notes_html = "<p>GoCue %s</p>" % escape(version)
    if args.notes:
        notes_html = pathlib.Path(args.notes).read_text(encoding="utf-8")
    notes_html = wrap_notes_for_feed(notes_html)

    appcast = output_dir / "appcast.xml"
    url, appcast_url = write_appcast(appcast, args.repo, version, installer, signature, notes_html)

    print("installer :", installer)
    print("appcast   :", appcast)
    print("download  :", url)
    print("feed      :", appcast_url)

    if args.publish:
        gh = find_gh()
        cmd = [gh, "release", "create", "v" + version, str(installer), str(appcast),
               "--repo", args.repo, "--title", "Enqueue " + version]
        cmd += ["--notes-file", args.notes] if args.notes else ["--generate-notes"]
        run(cmd)
        print("published :", "https://github.com/%s/releases/tag/v%s" % (args.repo, version))

        # the same installer under a fixed name: /releases/latest/download/GoCue-Setup.exe always gives the newest
        fixed = output_dir / FIXED_INSTALLER_NAME
        shutil.copyfile(installer, fixed)
        run([gh, "release", "upload", "v" + version, str(fixed), "--repo", args.repo, "--clobber"])

        if not args.skip_site:
            deploy_site(args.repo, {
                "version": version, "tag": "v" + version, "url": url, "size": installer.stat().st_size,
                "date": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                "latest_url": "https://github.com/%s/releases/latest/download/%s" % (args.repo, FIXED_INSTALLER_NAME),
                "feedback": read_feedback_link()})
    else:
        print("not published (add --publish to create the GitHub release)")


if __name__ == "__main__":
    main()
