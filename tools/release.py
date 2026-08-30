#!/usr/bin/env python3
"""GoCue release helper (Windows).

Pipeline:  build (Release) -> unit tests -> Inno Setup installer -> EdDSA signature
           -> appcast.xml -> (optional) GitHub Release upload via gh.

Typical use on the release machine:

    python tools/release.py --repo owner/gocue --key C:/keys/gocue_eddsa_priv.pem --publish

Environment fallbacks: GOCUE_GITHUB_REPO, GOCUE_EDDSA_PRIVATE_KEY_FILE, WINSPARKLE_DIR, ISCC, GH.
The version comes from project(GoCue VERSION x.y.z) in CMakeLists.txt.
"""
import argparse
import datetime
import email.utils
import os
import pathlib
import re
import subprocess
import sys
from xml.sax.saxutils import escape

ROOT = pathlib.Path(__file__).resolve().parents[1]


def read_version():
    text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(r"project\(GoCue\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)", text)
    if not match:
        sys.exit("could not find project(GoCue VERSION x.y.z) in CMakeLists.txt")
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
         "/DOutputDir=" + str(output_dir), str(ROOT / "installer" / "GoCue.iss")])
    installer = output_dir / ("GoCue-Setup-%s.exe" % version)
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


def write_appcast(path, repo, version, installer, signature, notes_html):
    url = "https://github.com/%s/releases/download/v%s/%s" % (repo, version, installer.name)
    appcast_url = "https://github.com/%s/releases/latest/download/appcast.xml" % repo
    pub_date = email.utils.format_datetime(datetime.datetime.now(datetime.timezone.utc))
    length = installer.stat().st_size
    xml = """<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle" xmlns:dc="http://purl.org/dc/elements/1.1/">
  <channel>
    <title>GoCue updates</title>
    <link>%s</link>
    <description>GoCue release feed</description>
    <language>ko</language>
    <item>
      <title>GoCue %s</title>
      <pubDate>%s</pubDate>
      <description><![CDATA[%s]]></description>
      <enclosure url="%s"
                 sparkle:version="%s"
                 sparkle:os="windows"
                 length="%d"
                 type="application/octet-stream"
                 sparkle:edSignature="%s" />
    </item>
  </channel>
</rss>
""" % (escape(appcast_url), escape(version), pub_date, notes_html, escape(url), escape(version), length, escape(signature))
    path.write_text(xml, encoding="utf-8")
    return url, appcast_url


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
    args = parser.parse_args()

    if not args.repo:
        sys.exit("--repo owner/name (or GOCUE_GITHUB_REPO) is required for the appcast URLs")

    # Single source of truth: the exe's VERSIONINFO, the installer name, the appcast and the
    # GitHub tag must all agree, otherwise the published appcast points at a 404.
    version = read_version()
    tag = os.environ.get("GITHUB_REF_NAME", "")
    if tag and tag != "v" + version:
        sys.exit("git tag %s does not match project(GoCue VERSION %s) - bump CMakeLists.txt or re-tag" % (tag, version))

    source_dir = ROOT / "build" / "vs2022" / "GoCue_artefacts" / "Release"
    output_dir = ROOT / "installer" / "output"

    if not args.skip_build:
        build(args.preset, args.skip_tests)

    for required in ("GoCue.exe", "WinSparkle.dll"):
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

    appcast = output_dir / "appcast.xml"
    url, appcast_url = write_appcast(appcast, args.repo, version, installer, signature, notes_html)

    print("installer :", installer)
    print("appcast   :", appcast)
    print("download  :", url)
    print("feed      :", appcast_url)

    if args.publish:
        gh = find_gh()
        cmd = [gh, "release", "create", "v" + version, str(installer), str(appcast),
               "--repo", args.repo, "--title", "GoCue " + version]
        cmd += ["--notes-file", args.notes] if args.notes else ["--generate-notes"]
        run(cmd)
        print("published :", "https://github.com/%s/releases/tag/v%s" % (args.repo, version))
    else:
        print("not published (add --publish to create the GitHub release)")


if __name__ == "__main__":
    main()
