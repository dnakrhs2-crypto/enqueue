#!/usr/bin/env python3
"""Enqueue release helper (Windows).

Pipeline:  build (Release) -> unit tests -> Inno Setup installer -> EdDSA signature
           -> appcast.xml -> (optional) GitHub Release upload via gh -> website (gh-pages).

The website (site/) is deployed to the gh-pages branch with latest.json (installer URL, size, date) and
notes.html (every docs/release-notes/*.html, newest first).  `--site-only` redeploys it from the latest
GitHub release without building anything.

Typical use on the release machine:

    python tools/release.py --repo owner/enqueue --key C:/keys/gocue_eddsa_priv.pem --publish

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


def recorder_identity():
    text = (ROOT / "recorder/src/app/ProductIdentity.h").read_text(encoding="utf-8")
    return dict(re.findall(r'^#define RECORDER_([A-Z_]+) "([^"\r\n]*)"', text, re.M))


def recorder_app():
    identity = recorder_identity()
    return dict(name=identity["DISPLAY_NAME"], exe=identity["PACKAGE_STEM"] + ".exe", iss="Recorder.iss",
                target="Recorder", fixed=identity["PACKAGE_STEM"] + "-Setup.exe",
                notes_dir="docs/release-notes/recorder", site_dir=identity["SITE_DIR"],
                tag_prefix=identity["TAG_PREFIX"], repo=identity["RELEASE_REPO"], remote=identity["RELEASE_REMOTE"],
                appcast=identity["APPCAST_URL"], publication_confirmed=identity["PUBLICATION_CONFIRMED"] == "1")


APPS = {
    "enqueue": dict(name="Enqueue", exe="Enqueue.exe", iss="Enqueue.iss", artefacts="Enqueue_artefacts", target="Enqueue",
                    fixed="Enqueue-Setup.exe", notes_dir="docs/release-notes", site_dir="", tag_prefix="v",
                    repo="dnakrhs2-crypto/enqueue", remote="origin", ctest_filter="^EnqueueUnitTests$"),
    "livemix": dict(name="LiveMix", exe="LiveMix.exe", iss="LiveMix.iss", artefacts="LiveMix_artefacts", target="LiveMix",
                    fixed="LiveMix-Setup.exe", notes_dir="docs/release-notes/livemix", site_dir="livemix", tag_prefix="livemix-v",
                    repo="dnakrhs2-crypto/livemix", remote="livemix", ctest_filter="^EnqueueUnitTests$"),
    "recorder": recorder_app(),
}
SITE_REPO = "dnakrhs2-crypto/enqueue"   # 곰튀김.com lives on this repo's gh-pages, both apps included
APP = APPS["enqueue"]


def read_version():
    if APP.get("target") == "Recorder":
        return recorder_identity()["VERSION"]
    text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    if APP["name"] == "LiveMix":
        match = re.search(r'set\(LIVEMIX_VERSION\s+"([0-9]+\.[0-9]+\.[0-9]+)"', text)
        if not match:
            sys.exit("could not find set(LIVEMIX_VERSION \"x.y.z\") in CMakeLists.txt")
        return match.group(1)
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
    run(["cmake", "--build", "--preset", preset + "-release", "--target", APP["target"], "EnqueueTests", "--", "-m", "-v:m", "-nologo"])
    if not skip_tests:
        # only this app's suites: the Recorder tests registered by recorder/CMakeLists.txt need RecorderTests.exe, which an
        # Enqueue / LiveMix build does not make (2026-09-14: the 0.10.2 release stopped on 41 "Not Run" Recorder tests)
        run(["ctest", "--preset", preset + "-release"] + (["-R", APP["ctest_filter"]] if APP.get("ctest_filter") else []))


def make_installer(iscc, version, source_dir, output_dir, tools_dir=""):
    output_dir.mkdir(parents=True, exist_ok=True)
    defines = ["/DAppVersion=" + version, "/DSourceDir=" + str(source_dir), "/DOutputDir=" + str(output_dir)]
    if tools_dir and os.path.isdir(tools_dir):
        defines.append("/DToolsDir=" + str(tools_dir))   # Enqueue: the YouTube download tools go into {app}\tools
    run([iscc, "/Q"] + defines + [str(ROOT / "installer" / APP["iss"])])
    installer = output_dir / ("%s-Setup-%s.exe" % (APP["name"], version))
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
    url = "https://github.com/%s/releases/download/%s%s/%s" % (repo, APP["tag_prefix"], version, installer.name)
    appcast_url = "https://github.com/%s/releases/latest/download/appcast.xml" % repo
    pub_date = email.utils.format_datetime(datetime.datetime.now(datetime.timezone.utc))
    length = installer.stat().st_size
    xml = """<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle" xmlns:dc="http://purl.org/dc/elements/1.1/">
  <channel>
    <title>%s updates</title>
    <link>%s</link>
    <description>%s release feed</description>
    <language>ko</language>
    <item>
      <title>%s %s</title>
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
""" % (APP["name"], escape(appcast_url), APP["name"], APP["name"], escape(version), pub_date, notes_html, escape(url), escape(version), length, escape(signature))
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


def build_notes_page(site_dir, latest_version, app):
    """notes.html: the release notes up to the published version, newest first, in the site's style."""
    notes_dir = ROOT / app["notes_dir"]
    page_dir = site_dir / app["site_dir"] if app["site_dir"] else site_dir
    css = "../style.css" if app["site_dir"] else "style.css"
    files = sorted((f for f in notes_dir.glob("*.html") if version_key(f.stem) <= version_key(latest_version)),
                   key=lambda p: version_key(p.stem), reverse=True)
    parts = []
    for f in files:
        fragment = f.read_text(encoding="utf-8").strip()
        parts.append('<article class="notes">%s</article>' % fragment)
    page = """<!doctype html>
<html lang="ko">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>%s 바뀐 점</title>
<link rel="icon" href="assets/icon.png">
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Noto+Sans+KR:wght@400;500;700&display=swap">
<link rel="stylesheet" href="%s">
<script defer src="https://static.cloudflareinsights.com/beacon.min.js" data-cf-beacon='{"token": "__CF_BEACON_TOKEN__"}'></script>
</head>
<body>
<header class="wrap top">
  <a class="brand" href="./"><img src="assets/icon.png" alt="" width="36" height="36">%s</a>
  <nav class="nav"><a href="./">다운로드</a><a href="https://github.com/%s/releases">GitHub</a></nav>
</header>
<main class="wrap">
  <h1 style="font-size:28px;margin-top:12px">바뀐 점</h1>
  <p class="meta" style="margin-top:6px">버전별 변경 사항. 최신 버전이 맨 위입니다.</p>
  %s
</main>
<footer class="wrap"><div>%s · 곰튀김</div></footer>
</body>
</html>
""" % (app["name"], css, app["name"], app["repo"], "\n  ".join(parts), app["name"])
    page_dir.mkdir(parents=True, exist_ok=True)
    (page_dir / "notes.html").write_text(page, encoding="utf-8")


REPO_FOR_SITE = ""


def apply_release_links(pages, latest, app):
    """The landing page gets the current installer's direct URL baked in (index.html placeholders), so the button
    is a real, counted download even before latest.json is fetched; the JSON fetch carries the version as a cache key."""
    page_dir = pages / app["site_dir"] if app["site_dir"] else pages
    page = page_dir / "index.html"
    url = latest.get("url", "") if latest else ""
    if latest and "/releases/download/" not in url:
        sys.exit("latest url is not a direct asset link: %s" % url)
    text = page.read_text(encoding="utf-8")
    if latest:
        text = (text.replace("__DOWNLOAD_URL__", url)
                    .replace("__DOWNLOAD_LABEL__", "%s %s 다운로드" % (app["name"], latest.get("version", "")))
                    .replace("__SITE_REV__", latest.get("version", "0")))
    else:
        # no release yet: the button points at the releases page
        text = (text.replace("__DOWNLOAD_URL__", "https://github.com/%s/releases" % app["repo"])
                    .replace("__DOWNLOAD_LABEL__", "%s 다운로드 (곧 공개)" % app["name"])
                    .replace("__SITE_REV__", "0"))
    page.write_text(text, encoding="utf-8")


def apply_analytics(pages):
    """Cloudflare Web Analytics: site/cf-beacon-token.txt holds the beacon token (public, it sits in the page anyway).
    Empty or missing = the beacon line is dropped from every page."""
    token_file = ROOT / "site" / "cf-beacon-token.txt"
    token = token_file.read_text(encoding="utf-8").strip() if token_file.is_file() else ""
    for page in pages.rglob("*.html"):
        text = page.read_text(encoding="utf-8")
        if "__CF_BEACON_TOKEN__" not in text:
            continue
        if token:
            text = text.replace("__CF_BEACON_TOKEN__", token)
        else:
            text = "\n".join(line for line in text.split("\n") if "__CF_BEACON_TOKEN__" not in line)
        page.write_text(text, encoding="utf-8")
    token_copy = pages / "cf-beacon-token.txt"
    if token_copy.exists():
        token_copy.unlink()


def deploy_site(repo, latest):
    """Copy site/ plus latest.json and notes.html onto the gh-pages branch and push it. 'latest' is the app being
    released (or None for --site-only); the other app's latest is read from its GitHub releases."""
    global REPO_FOR_SITE
    REPO_FOR_SITE = repo
    latest_by_app = {}
    site_apps = {key: app for key, app in APPS.items() if app.get("publication_confirmed", True)}
    for key, app in site_apps.items():
        if latest is not None and app is APP:
            latest_by_app[key] = latest
        else:
            latest_by_app[key] = latest_from_github(find_gh(), app["repo"], app, allow_missing=True)
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
        unpublished = {app["site_dir"] for app in APPS.values() if not app.get("publication_confirmed", True)}
        shutil.copytree(site_src, pages, dirs_exist_ok=True,
                        ignore=lambda directory, names: unpublished.intersection(names) if pathlib.Path(directory) == site_src else set())
        (pages / ".nojekyll").write_text("", encoding="utf-8")
        for key, app in site_apps.items():
            info = latest_by_app.get(key)
            page_dir = pages / app["site_dir"] if app["site_dir"] else pages
            page_dir.mkdir(parents=True, exist_ok=True)
            if info:
                (page_dir / "latest.json").write_text(json.dumps(info, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
            build_notes_page(pages, info.get("version", "0.0.0") if info else "0.0.0", app)
            apply_release_links(pages, info, app)
        apply_analytics(pages)
        run(["git", "add", "-A"], cwd=pages)
        status = run(["git", "status", "--porcelain"], cwd=pages, capture=True)
        if not status.strip():
            print("site      : unchanged")
            return
        run(["git", "-c", "user.name=Enqueue release", "-c", "user.email=release@gocue.invalid",
             "commit", "--quiet", "-m", "site: %s %s" % (APP["name"], (latest or {}).get("version", "refresh"))], cwd=pages)
        run(["git", "push", "--quiet", "origin", "gh-pages"], cwd=pages)
        print("site      : https://%s.github.io/%s/" % tuple(repo.split("/", 1)))
    finally:
        shutil.rmtree(work, ignore_errors=True)


def latest_from_github(gh, repo, app=None, allow_missing=False):
    """latest.json content for --site-only: read from the newest GitHub release (None when the repo has none yet)."""
    app = app or APP
    try:
        out = run([gh, "release", "view", "--repo", repo, "--json", "tagName,publishedAt,assets"], capture=True)
    except subprocess.CalledProcessError as e:
        stderr = (e.stderr or "").strip()
        if allow_missing and "release not found" in stderr.lower():
            print("no release yet on", repo)
            return None
        sys.exit("gh release view failed for %s (%s) - the site was not touched" % (repo, stderr or e))
    info = json.loads(out)
    version = info["tagName"]
    if version.startswith(app["tag_prefix"]):
        version = version[len(app["tag_prefix"]):]
    version = version.lstrip("v")
    installer = next((a for a in info["assets"] if re.match(r"(Enqueue|GoCue|LiveMix|Recorder|Tally)-Setup-[0-9.]+\.exe$", a["name"])), None)
    if installer is None:
        sys.exit("the latest release has no %s-Setup-x.y.z.exe asset" % app["name"])
    if "/releases/download/" not in installer["url"]:
        sys.exit("asset url is not a browser download link: %s" % installer["url"])
    # the fixed-name link only exists when that asset is on the latest release (GoCue-Setup.exe before the rename)
    fixed = next((a for a in info["assets"] if a["name"] in (app["fixed"], "GoCue-Setup.exe")), None)
    latest_url = ("https://github.com/%s/releases/latest/download/%s" % (repo, fixed["name"])) if fixed else installer["url"]
    return {"version": version, "tag": info["tagName"], "url": installer["url"], "size": installer["size"],
            "date": info["publishedAt"], "latest_url": latest_url, "feedback": read_feedback_link()}


def package_legacy(args, version):
    """Opt-in local mode; the existing legacy release path below is unchanged."""
    build(args.preset, False)
    source = ROOT / "build/vs2022" / APP["artefacts"] / "Release"
    for name in (APP["exe"], "WinSparkle.dll"):
        if not (source / name).is_file():
            sys.exit("missing package input: " + str(source / name))
    output = ROOT / "out/release" / args.app
    output.mkdir(parents=True, exist_ok=True)
    output = pathlib.Path(tempfile.mkdtemp(prefix=version + "-", dir=output))
    iscc, tool = find_iscc(), find_winsparkle_tool(args.winsparkle_dir)
    if not iscc or not tool or not pathlib.Path(args.key).is_file():
        sys.exit("local packaging needs ISCC, WinSparkle and --key")
    if args.app == "enqueue":
        for name in ("yt-dlp.exe", "qjs.exe", "lame.exe", "libsndfile-1.dll", "LICENSES.txt"):
            if not (pathlib.Path(args.tools_dir) / name).is_file():
                sys.exit("missing Enqueue tools: " + name)
    installer = make_installer(iscc, version, source, output, args.tools_dir if args.app == "enqueue" else "")
    signature = sign(tool, args.key, installer)
    notes_path = pathlib.Path(args.notes) if args.notes else ROOT / APP["notes_dir"] / (version + ".html")
    notes = notes_path.read_text(encoding="utf-8") if notes_path.is_file() else "<p>%s %s</p>" % (APP["name"], version)
    (output / "notes.html").write_text(wrap_notes_for_feed(notes), encoding="utf-8")
    url, _ = write_appcast(output / "appcast.xml", args.repo, version, installer, signature, wrap_notes_for_feed(notes))
    (output / "latest.json").write_text(json.dumps({"version": version, "tag": APP["tag_prefix"] + version,
        "url": url, "size": installer.stat().st_size, "publishable": False}, indent=2) + "\n", encoding="utf-8")
    print("local package (not published):", output)


def recorder_run(cmd, cwd=ROOT, capture=False):
    # Windows sessions can contain both Path and PATH. An explicit environment
    # avoids MSBuild's duplicate-key error without changing the legacy runner.
    print("+", " ".join(str(c) for c in cmd), flush=True)
    environment = dict(os.environ)
    environment["MSBUILDDISABLENODEREUSE"] = "1"
    result = subprocess.run([str(c) for c in cmd], cwd=str(cwd), check=True, env=environment,
                            capture_output=capture, text=True, encoding="utf-8", errors="replace")
    return result.stdout if capture else ""


def recorder_build(args, public_key):
    if not re.fullmatch(r"[A-Za-z0-9_-]+", args.preset):
        sys.exit("Recorder preset must be a simple preset name")
    cmake = os.environ.get("CMAKE", "") or shutil.which("cmake")
    if not cmake:
        sys.exit("cmake not found (add CMake/bin to PATH or set CMAKE to cmake.exe)")
    ctest = pathlib.Path(cmake).with_name("ctest.exe" if os.name == "nt" else "ctest")
    build_dir = ROOT / "out/recorder-package-build" / args.preset
    # A skipped/removed target must not reuse a previous configure's description.
    for stale in build_dir.rglob("recorder-package-Release.json"):
        if not stale.resolve().is_relative_to(build_dir.resolve()):
            sys.exit("packaging description escapes the isolated build tree")
        stale.unlink()
    recorder_run([cmake, "--preset", args.preset, "-B", build_dir, "-DGOCUE_BUILD_RECORDER=ON",
                  "-DRECORDER_UPDATE_PUBLIC_KEY=" + public_key])
    # ALL and all registered CTests pick up later merged Recorder/probe/test targets.
    recorder_run([cmake, "--build", build_dir, "--config", "Release", "--parallel",
                  os.environ.get("CMAKE_BUILD_PARALLEL_LEVEL", "4")])
    recorder_run([ctest, "--test-dir", build_dir, "-C", "Release", "--output-on-failure", "--no-tests=error"])
    recorder_run([sys.executable, "-B", "-m", "unittest", "discover", "-s", "tools/recorder/tests"])
    descriptions = list(build_dir.rglob("recorder-package-Release.json"))
    if len(descriptions) != 1:
        sys.exit("Recorder packaging description missing/ambiguous: configure must enable the Recorder target")
    description = json.loads(descriptions[0].read_text(encoding="utf-8"))
    executable = pathlib.Path(description["exe"])
    if not executable.resolve().is_relative_to(build_dir.resolve()) or not executable.is_file():
        sys.exit("CMake Recorder executable is missing or outside the isolated build tree")
    if description["public_key"] != public_key:
        sys.exit("CMake embedded Recorder public key differs from signing key")
    return description


def stage_recorder(description, output, identity):
    from recorder.audit_ffmpeg import audit, dependency_closure, pe_imports, sha256, write_json
    from recorder.validate_release import REQUIRED_NOTICES, file_record
    lock_path = ROOT / "recorder/third_party/ffmpeg.lock.json"
    lock = json.loads(lock_path.read_text(encoding="utf-8"))
    audited = audit(description["ffmpeg_root"], lock, lock["runtime"]["version"])
    if audited["status"] != "PASS":
        sys.exit("Recorder FFmpeg audit failed: " + str(audited["errors"]))
    declared = pathlib.Path(description["exe"])
    if declared.is_symlink() or (hasattr(declared, "is_junction") and declared.is_junction()):
        sys.exit("declared Recorder executable is a symlink/junction: " + str(declared))
    executable = declared.resolve()
    source = declared.parent
    payload = output / "payload"
    payload.mkdir()
    # Copy this target's runtime directory, never a guessed build/artefacts path.
    # Future resources are included; linker/debug intermediates are not runtime assets.
    excluded = {".pdb", ".ilk", ".lib", ".exp", ".obj", ".iobj", ".ipdb", ".log", ".tlog"}
    for path in source.rglob("*"):
        if path.is_symlink() or (hasattr(path, "is_junction") and path.is_junction()):
            sys.exit("runtime directory contains a symlink/junction: " + str(path))
        if path.is_file() and path.suffix.lower() not in excluded:
            # The reused package build directory keeps executables from earlier configurations (Recorder.exe after
            # the Tally rename): only the executable CMake declared for this target is a runtime asset.
            if path.suffix.lower() == ".exe" and path.resolve() != executable:
                print("staging   : skipping stale executable " + path.name)
                continue
            destination = payload / path.relative_to(source)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, destination)
    for relative, record in lock["files"]["dlls"].items():
        path = payload / pathlib.Path(relative).name
        if not path.is_file() or sha256(path) != record["sha256"]:
            sys.exit("built FFmpeg DLL missing/differs from lock: " + relative)
    if not (payload / "WinSparkle.dll").is_file():
        sys.exit("Recorder packaging requires WinSparkle.dll (configure WINSPARKLE_DIR)")
    if sha256(payload / "WinSparkle.dll") != sha256(description["winsparkle_dll"]):
        sys.exit("built WinSparkle DLL differs from the configured SDK")
    images = {p.relative_to(payload).as_posix(): pe_imports(p) for p in payload.rglob("*")
              if p.is_file() and p.suffix.lower() in (".exe", ".dll")}
    closure = dependency_closure(images)
    if closure["missing"]:
        sys.exit("missing runtime dependency DLLs: " + str(closure["missing"]))
    shutil.copytree(ROOT / "recorder/licenses", payload / "licenses")
    for name in REQUIRED_NOTICES:
        if not (payload / "licenses" / name).is_file():
            sys.exit("missing required notice: " + name)
    # Bind the WinSparkle notices to the actual SDK used by the generated target.
    for original, copied in (("COPYING", "WinSparkle-COPYING.txt"), ("COPYING.expat", "WinSparkle-COPYING.expat.txt")):
        if (pathlib.Path(description["winsparkle_dir"]) / original).read_bytes() != (payload / "licenses" / copied).read_bytes():
            sys.exit("WinSparkle notice differs from configured SDK: " + original)
    source_url = identity["RELEASE_BASE_URL"] + identity["TAG_PREFIX"] + identity["VERSION"] + "/" + identity["SOURCES_STEM"] + "-" + identity["VERSION"] + ".zip"
    write_json(payload / "release-links.json", {"version": identity["VERSION"], "source_url": source_url,
        "publication_confirmed": identity["PUBLICATION_CONFIRMED"] == "1"})
    shutil.copyfile(lock_path, output / "ffmpeg.lock.json")
    write_json(output / "ffmpeg-audit.json", audited)
    return payload, [file_record(p, output) for p in sorted(payload.rglob("*")) if p.is_file()]


def write_recorder_installer_identity(path, identity):
    values = {"AppName": identity["DISPLAY_NAME"], "AppExe": identity["PACKAGE_STEM"] + ".exe",
              "AppPublisher": identity["COMPANY"], "InstallerAppId": "{" + identity["APP_ID"],
              "SettingsFolder": identity["SETTINGS_FOLDER"], "ProjectExtension": identity["PROJECT_EXTENSION"],
              "FileType": identity["FILE_TYPE"], "IdentityVersion": identity["VERSION"], "PackageStem": identity["PACKAGE_STEM"]}
    if any('"' in value or "\n" in value or "\r" in value for value in values.values()):
        sys.exit("invalid Inno ProductIdentity value")
    path.write_text("\n".join('#define %s "%s"' % (key, value) for key, value in values.items()) + "\n", encoding="utf-8-sig")


def write_recorder_metadata(output, identity, installer, signature, notes_path, public_key, source_archive=None):
    from recorder.audit_ffmpeg import write_json
    from recorder.validate_release import file_record
    version = identity["VERSION"]
    notes = wrap_notes_for_feed(notes_path.read_text(encoding="utf-8"))
    (output / "notes.html").write_text(notes, encoding="utf-8")
    appcast = output / "appcast.xml"
    # Keep write_appcast's legacy output unchanged; adapt only this candidate file.
    old_url, old_feed = write_appcast(appcast, identity["RELEASE_REPO"], version, installer, signature, notes)
    url = identity["RELEASE_BASE_URL"] + identity["TAG_PREFIX"] + version + "/" + installer.name
    appcast.write_text(appcast.read_text(encoding="utf-8").replace(escape(old_url), escape(url))
                      .replace(escape(old_feed), escape(identity["APPCAST_URL"])), encoding="utf-8")
    latest = {"version": version, "tag": identity["TAG_PREFIX"] + version, "url": url,
              "size": installer.stat().st_size, "date": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
              "publishable": False, "source_url": identity["RELEASE_BASE_URL"] + identity["TAG_PREFIX"] + version + "/"
                + identity["SOURCES_STEM"] + "-" + version + ".zip", "notices_url": "licenses/NOTICE.txt"}
    write_json(output / "latest.json", latest)
    (output / "public-key.txt").write_text(public_key + "\n", encoding="ascii")
    # Byte-preserved legacy pages; only the new Recorder subtree gets generated files.
    shutil.copytree(ROOT / "site", output / "site")
    page_dir = output / "site" / identity["SITE_DIR"]
    page_dir.mkdir(parents=True, exist_ok=True)
    write_json(page_dir / "latest.json", latest)
    shutil.copyfile(output / "notes.html", page_dir / "notes.html")
    shutil.copytree(output / "payload/licenses", page_dir / "licenses", dirs_exist_ok=True)
    manifest = {"schema_version": 1, "app": "recorder", "version": version, "identity": identity,
                "installer": installer.name, "executable": "payload/" + identity["PACKAGE_STEM"] + ".exe",
                "embedded_public_key": public_key, "source_archive": source_archive,
                "publication_mode": "package-only", "files": []}
    manifest["files"] = [file_record(path, output) for path in sorted(output.rglob("*"))
                         if path.is_file() and path not in (output / "manifest.json", output / "validation.json")]
    write_json(output / "manifest.json", manifest)
    return manifest


def package_recorder(args):
    from recorder.audit_ffmpeg import write_json
    from recorder.validate_release import file_record, validate_bundle, validate_sources
    identity = recorder_identity()
    iscc = find_iscc()
    # Configure-preset WinSparkle path is used later; preflight needs an explicit
    # tool path or its environment fallback, without reading any private key itself.
    tool = find_winsparkle_tool(args.winsparkle_dir)
    if not tool:
        # Query local preset inheritance without configuring the user's build tree.
        presets = {}
        for name in ("CMakePresets.json", "CMakeUserPresets.json"):
            file = ROOT / name
            if file.is_file():
                presets.update({p["name"]: p for p in json.loads(file.read_text(encoding="utf-8"))["configurePresets"]})
        def sdk_path(name, seen=None):
            seen = set() if seen is None else seen
            if name in seen:
                return ""
            seen.add(name)
            preset = presets.get(name, {})
            value = preset.get("cacheVariables", {}).get("WINSPARKLE_DIR", "")
            if value:
                return value.get("value", "") if isinstance(value, dict) else value
            parents = preset.get("inherits", [])
            for parent in ([parents] if isinstance(parents, str) else parents):
                value = sdk_path(parent, seen)
                if value:
                    return value
            return ""
        tool = find_winsparkle_tool(sdk_path(args.preset))
    if not iscc or not tool:
        sys.exit("Recorder package-only needs ISCC and WinSparkle (ISCC / --winsparkle-dir / WINSPARKLE_DIR)")
    if not args.key or not pathlib.Path(args.key).is_file():
        sys.exit("EdDSA private key file not found (--key / GOCUE_EDDSA_PRIVATE_KEY_FILE); no package was built")
    public_output = recorder_run([tool, "public-key", "--private-key-file", args.key], capture=True)
    keys = re.findall(r"(?<![A-Za-z0-9+/])[A-Za-z0-9+/]{43}=(?![A-Za-z0-9+/=])", public_output)
    if len(set(keys)) != 1:
        sys.exit("could not read exactly one EdDSA public key from winsparkle-tool")
    public_key = keys[0]
    notes = pathlib.Path(args.notes) if args.notes else ROOT / APP["notes_dir"] / (identity["VERSION"] + ".html")
    if not notes.is_file():
        sys.exit("release notes missing: " + str(notes))
    if args.source_bundle:
        validate_sources(pathlib.Path(args.source_bundle))
    description = recorder_build(args, public_key)
    parent = ROOT / "out/release/recorder"
    parent.mkdir(parents=True, exist_ok=True)
    output = pathlib.Path(tempfile.mkdtemp(prefix=identity["VERSION"] + "-", dir=parent))
    payload, payload_before = stage_recorder(description, output, identity)
    identity_file = output / "ProductIdentity.iss"
    write_recorder_installer_identity(identity_file, identity)
    recorder_run([iscc, "/Q", "/DIdentityFile=" + str(identity_file), "/DSourceDir=" + str(payload),
                  "/DOutputDir=" + str(output), "/DAppVersion=" + identity["VERSION"], str(ROOT / "installer/Recorder.iss")])
    installer = output / (identity["PACKAGE_STEM"] + "-Setup-" + identity["VERSION"] + ".exe")
    if not installer.is_file():
        sys.exit("Recorder installer was not produced: " + str(installer))
    payload_after = [file_record(p, output) for p in sorted(payload.rglob("*")) if p.is_file()]
    if payload_after != payload_before:
        sys.exit("Recorder payload changed while compiling the installer")
    signature = sign(tool, args.key, installer)
    source_name = None
    if args.source_bundle:
        source_name = identity["SOURCES_STEM"] + "-" + identity["VERSION"] + ".zip"
        shutil.copyfile(args.source_bundle, output / source_name)
    write_recorder_metadata(output, identity, installer, signature, notes, public_key, source_name)
    report = validate_bundle(output, tool, public_key)
    write_json(output / "validation.json", report)
    if report["errors"]:
        sys.exit("Recorder candidate validation failed: " + str(report["errors"]))
    print("local Recorder candidate:", output)
    print("technical checks:", report["technical_status"], "; release gates:", report["status"])
    print("unresolved:", ", ".join(report["blockers"]))
    if not getattr(args, "publish", False):
        print("package-only: no GitHub, site or tag changes; this does not approve publication")
    return {"output": output, "installer": installer, "appcast": output / "appcast.xml", "notes": output / "notes.html",
            "version": identity["VERSION"], "tag": identity["TAG_PREFIX"] + identity["VERSION"],
            "url": identity["RELEASE_BASE_URL"] + identity["TAG_PREFIX"] + identity["VERSION"] + "/" + installer.name,
            "report": report, "source": (output / source_name) if source_name else None,
            "sources_name": identity["SOURCES_STEM"] + "-" + identity["VERSION"] + ".zip"}


def recorder_tag_preflight(tag_name):
    """Same checks the legacy path runs before its long build: an existing tag must be annotated and on HEAD."""
    head = run(["git", "rev-parse", "HEAD"], cwd=ROOT, capture=True).strip()
    if run(["git", "tag", "--list", tag_name], cwd=ROOT, capture=True).strip():
        tagged = run(["git", "rev-list", "-n", "1", tag_name], cwd=ROOT, capture=True).strip()
        if tagged != head:
            sys.exit("tag %s already exists on another commit (%s, HEAD is %s) - bump RECORDER_VERSION" % (tag_name, tagged[:10], head[:10]))
        if run(["git", "cat-file", "-t", "refs/tags/" + tag_name], cwd=ROOT, capture=True).strip() != "tag":
            sys.exit("tag %s is a lightweight tag - delete it and let the script create the annotated one" % tag_name)
    ref = os.environ.get("GITHUB_REF_NAME", "")
    if ref and ref != tag_name:
        sys.exit("git tag %s does not match the Recorder version (expected %s)" % (ref, tag_name))


def publish_recorder(args, candidate):
    """Same shape as the legacy publish: annotated tag at HEAD, push main + tag to the app remote (a mirror of this
    repository), GitHub release with installer + appcast (+ fixed-name installer), then the website."""
    tag_name = candidate["tag"]
    report = candidate.get("report") or {}
    if report.get("status") != "PASS":
        blockers = ", ".join(report.get("blockers", []))
        if not getattr(args, "accept_blocked_gates", False):
            sys.exit("Recorder candidate passed the technical checks but release gates are %s (%s). "
                     "Publishing needs the release owner's decision: re-run with --accept-blocked-gates." % (report.get("status"), blockers))
        print("publishing with unresolved release gates (accepted by the release owner):", blockers)
    # the About dialog links the same-release source archive: a validated --source-bundle, otherwise this commit's
    # snapshot; prepared before anything is tagged or pushed
    source = candidate.get("source")
    if source is None:
        source = candidate["output"] / candidate["sources_name"]
        run(["git", "archive", "--format=zip", "-o", str(source), "HEAD"], cwd=ROOT)
    head = run(["git", "rev-parse", "HEAD"], cwd=ROOT, capture=True).strip()
    if run(["git", "tag", "--list", tag_name], cwd=ROOT, capture=True).strip():
        tagged = run(["git", "rev-list", "-n", "1", tag_name], cwd=ROOT, capture=True).strip()
        if tagged != head:
            sys.exit("tag %s is on %s, HEAD is %s - bump RECORDER_VERSION" % (tag_name, tagged[:10], head[:10]))
    else:
        run(["git", "tag", "-a", tag_name, "-m", APP["name"] + " " + candidate["version"]], cwd=ROOT)
    run(["git", "push", APP["remote"], "HEAD:main"], cwd=ROOT)
    run(["git", "push", APP["remote"], tag_name], cwd=ROOT)
    gh = find_gh()
    run([gh, "release", "create", tag_name, str(candidate["installer"]), str(candidate["appcast"]), str(source),
         "--repo", APP["repo"], "--title", APP["name"] + " " + candidate["version"], "--verify-tag",
         "--notes-file", str(candidate["notes"])])
    print("published :", "https://github.com/%s/releases/tag/%s" % (APP["repo"], tag_name))
    fixed = candidate["output"] / APP["fixed"]
    shutil.copyfile(candidate["installer"], fixed)
    run([gh, "release", "upload", tag_name, str(fixed), "--repo", APP["repo"], "--clobber"])
    if not args.skip_site:
        deploy_site(SITE_REPO, {
            "version": candidate["version"], "tag": tag_name, "url": candidate["url"], "size": candidate["installer"].stat().st_size,
            "date": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "latest_url": "https://github.com/%s/releases/latest/download/%s" % (APP["repo"], APP["fixed"]),
            "feedback": read_feedback_link()})


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--app", default="enqueue", choices=sorted(APPS), help="which app to release (default: enqueue)")
    parser.add_argument("--preset", default="local", help="CMake configure preset name (default: local)")
    parser.add_argument("--repo", default="", help="GitHub owner/repo for the release (default: the app's own; GOCUE_GITHUB_REPO applies to Enqueue only)")
    parser.add_argument("--key", default=os.environ.get("GOCUE_EDDSA_PRIVATE_KEY_FILE", ""), help="EdDSA private key file (winsparkle-tool generate-key)")
    parser.add_argument("--winsparkle-dir", default="", help="WinSparkle package dir (default: WINSPARKLE_DIR)")
    parser.add_argument("--notes", default="", help="release notes file (HTML or plain text)")
    parser.add_argument("--tools-dir", default=os.environ.get("ENQUEUE_TOOLS_DIR", "C:/Users/claude/SDKs/gocue_release/enqueue_tools"),
                        help="folder with yt-dlp.exe, qjs.exe, lame.exe (+ libsndfile-1.dll) bundled into the Enqueue installer (default: ENQUEUE_TOOLS_DIR)")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--skip-tests", action="store_true")
    parser.add_argument("--publish", action="store_true", help="create the GitHub release with gh and upload installer + appcast")
    parser.add_argument("--site-only", action="store_true", help="only redeploy the website from the latest GitHub release")
    parser.add_argument("--skip-site", action="store_true", help="publish without touching the website")
    parser.add_argument("--package-only", action="store_true", help="build/test/sign a local candidate; never change GitHub, site or tags")
    parser.add_argument("--source-bundle", default="", help="Recorder exact-source ZIP with source-manifest.json (optional for a blocked local candidate)")
    parser.add_argument("--allow-dirty", action="store_true", help="release from a working tree with uncommitted changes (not for real releases)")
    parser.add_argument("--accept-blocked-gates", action="store_true", help="Recorder --publish: publish a technically PASS candidate whose release gates are still BLOCKED (release owner decision)")
    args = parser.parse_args()
    global APP
    APP = APPS[args.app]

    if args.package_only and (args.publish or args.site_only or args.skip_build or args.skip_tests):
        parser.error("--package-only requires build/tests and cannot be combined with --publish or --site-only")
    if args.app == "recorder":
        # No recorder operation may fall through to the legacy publishing pipeline.
        # A CLI --repo override must not turn a provisional identity into a public release.
        if (args.publish or args.site_only) and not APP.get("publication_confirmed", False):
            sys.exit("Recorder publication is blocked: ProductIdentity/URLs require release-owner confirmation (RECORDER_PUBLICATION_CONFIRMED). Use --package-only.")
        if args.repo and args.repo != APP["repo"]:
            parser.error("Recorder --repo must match ProductIdentity.h; CLI overrides cannot change release identity")
        if args.site_only:
            deploy_site(SITE_REPO, None)
            return
        if args.package_only:
            package_recorder(args)
            return
        if not args.publish:
            parser.error("Recorder requires --package-only or --publish")
        if args.allow_dirty or args.skip_build or args.skip_tests:
            sys.exit("--publish builds and tests what is committed: --allow-dirty / --skip-build / --skip-tests are for local test builds only")
        dirty = run(["git", "status", "--porcelain"], cwd=ROOT, capture=True)
        if dirty.strip():
            sys.exit("the working tree has uncommitted changes - commit first:\n" + dirty)
        recorder_tag_preflight(APP["tag_prefix"] + recorder_identity()["VERSION"])
        publish_recorder(args, package_recorder(args))
        return

    if not args.repo:
        # the env fallback belongs to Enqueue: with --app livemix it would push to one repo and publish in another
        args.repo = (os.environ.get("GOCUE_GITHUB_REPO", "") if args.app == "enqueue" else "") or APP["repo"]

    if args.publish and (args.allow_dirty or args.skip_build or args.skip_tests):
        sys.exit("--publish builds and tests what is committed: --allow-dirty / --skip-build / --skip-tests are for local test builds only")

    if args.site_only:
        deploy_site(SITE_REPO, None)
        return

    # Single source of truth: the exe's VERSIONINFO, the installer name, the appcast and the
    # GitHub tag must all agree, otherwise the published appcast points at a 404.
    version = read_version()

    if args.package_only:
        package_legacy(args, version)
        return

    # what ships must be what is committed: a stray local edit would be in the installer but in no git history
    if not args.allow_dirty:
        dirty = run(["git", "status", "--porcelain"], cwd=ROOT, capture=True)
        if dirty.strip():
            sys.exit("the working tree has uncommitted changes - commit first (or --allow-dirty for a test build):\n" + dirty)

    # a tag that already exists must be this commit: a second "v0.9.4" on another commit would ship two different builds under one version
    if run(["git", "tag", "--list", APP["tag_prefix"] + version], cwd=ROOT, capture=True).strip():
        head = run(["git", "rev-parse", "HEAD"], cwd=ROOT, capture=True).strip()
        tagged = run(["git", "rev-list", "-n", "1", APP["tag_prefix"] + version], cwd=ROOT, capture=True).strip()
        if head != tagged:
            sys.exit("tag %s%s already exists on another commit (%s, HEAD is %s) - bump the version in CMakeLists.txt" % (APP["tag_prefix"], version, tagged[:10], head[:10]))

    tag = os.environ.get("GITHUB_REF_NAME", "")
    if tag and tag != APP["tag_prefix"] + version:
        sys.exit("git tag %s does not match the %s version %s (expected %s%s) - bump CMakeLists.txt or re-tag"
                 % (tag, APP["name"], version, APP["tag_prefix"], version))

    source_dir = ROOT / "build" / "vs2022" / APP["artefacts"] / "Release"
    output_dir = ROOT / "installer" / "output"

    if not args.skip_build:
        build(args.preset, args.skip_tests)

    for required in (APP["exe"], "WinSparkle.dll"):
        if not (source_dir / required).is_file():
            sys.exit("missing %s in %s (build Release with WINSPARKLE_DIR set)" % (required, source_dir))

    iscc = find_iscc()
    if iscc is None:
        sys.exit("ISCC.exe not found (install Inno Setup or set ISCC)")
    if APP["name"] == "Enqueue":
        # the YouTube download runs on the bundled tools: an installer without them would ship a dead feature
        required = ["yt-dlp.exe", "qjs.exe", "lame.exe", "libsndfile-1.dll", "LICENSES.txt"]
        missing = [f for f in required if not os.path.isfile(os.path.join(args.tools_dir, f))]
        if missing:
            sys.exit("Enqueue needs the YouTube download tools in --tools-dir (%s): missing %s" % (args.tools_dir, ", ".join(missing)))

    installer = make_installer(iscc, version, source_dir, output_dir, args.tools_dir if APP["name"] == "Enqueue" else "")

    tool = find_winsparkle_tool(args.winsparkle_dir)
    if tool is None:
        sys.exit("winsparkle-tool.exe not found (set --winsparkle-dir or WINSPARKLE_DIR)")
    if not args.key or not pathlib.Path(args.key).is_file():
        sys.exit("EdDSA private key file not found (--key / GOCUE_EDDSA_PRIVATE_KEY_FILE)")
    signature = sign(tool, args.key, installer)

    notes_html = "<p>%s %s</p>" % (APP["name"], escape(version))
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
        # the release is tied to one commit: an annotated tag at HEAD, pushed, and gh refuses to create the tag itself
        tag_name = APP["tag_prefix"] + version
        head = run(["git", "rev-parse", "HEAD"], cwd=ROOT, capture=True).strip()
        if run(["git", "tag", "--list", tag_name], cwd=ROOT, capture=True).strip():
            tagged = run(["git", "rev-list", "-n", "1", tag_name], cwd=ROOT, capture=True).strip()
            if tagged != head:
                sys.exit("tag %s is on %s, HEAD is %s - bump the version" % (tag_name, tagged[:10], head[:10]))
            if run(["git", "cat-file", "-t", "refs/tags/" + tag_name], cwd=ROOT, capture=True).strip() != "tag":
                sys.exit("tag %s is a lightweight tag - delete it and let the script create the annotated one" % tag_name)
        else:
            run(["git", "tag", "-a", tag_name, "-m", APP["name"] + " " + version], cwd=ROOT)
        # the release lives in the app's repo: LiveMix's is a mirror of this one (remote "livemix"); an overridden
        # --repo is pushed to by URL so the tag and the release land in the same place
        remote = APP["remote"] if args.repo == APP["repo"] else "https://github.com/%s.git" % args.repo
        run(["git", "push", remote, "HEAD:main"], cwd=ROOT)
        run(["git", "push", remote, tag_name], cwd=ROOT)

        gh = find_gh()
        cmd = [gh, "release", "create", tag_name, str(installer), str(appcast),
               "--repo", args.repo, "--title", APP["name"] + " " + version, "--verify-tag"]
        cmd += ["--notes-file", args.notes] if args.notes else ["--generate-notes"]
        run(cmd)
        print("published :", "https://github.com/%s/releases/tag/%s" % (args.repo, tag_name))

        # the same installer under a fixed name: /releases/latest/download/<App>-Setup.exe always gives the newest
        fixed = output_dir / APP["fixed"]
        shutil.copyfile(installer, fixed)
        run([gh, "release", "upload", tag_name, str(fixed), "--repo", args.repo, "--clobber"])

        if not args.skip_site:
            deploy_site(SITE_REPO, {
                "version": version, "tag": tag_name, "url": url, "size": installer.stat().st_size,
                "date": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                "latest_url": "https://github.com/%s/releases/latest/download/%s" % (args.repo, APP["fixed"]),
                "feedback": read_feedback_link()})
    else:
        print("not published (add --publish to create the GitHub release)")


if __name__ == "__main__":
    main()
