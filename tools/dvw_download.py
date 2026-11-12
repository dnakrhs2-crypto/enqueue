"""Encrypt a DVW installer and publish only the dvw-download branch.

Local check: publish installer.exe --dry-run --out empty-folder --password-file file
"""

import argparse
import base64
from datetime import datetime, timedelta, timezone
import hashlib
from http.client import HTTPException
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from urllib.request import Request, urlopen

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.kdf.pbkdf2 import PBKDF2HMAC


PART_SIZE = 16 * 1024 * 1024
ITERATIONS = 600_000
CHECK_TEXT = b"DVW-DOWNLOAD-V1"
PASSWORD_FILE = Path(r"C:\Users\claude\SDKs\gocue_release\dvw_download_password.txt")
REPOSITORY = "https://github.com/dnakrhs2-crypto/enqueue.git"
BASE_URL = "https://raw.githubusercontent.com/dnakrhs2-crypto/enqueue/dvw-download/"
KST = timezone(timedelta(hours=9))


def read_password(path):
    try:
        password = path.read_text(encoding="utf-8").strip()
    except (OSError, UnicodeError):
        # A decoding error can include password bytes; never print that exception.
        raise ValueError("Cannot read the UTF-8 password file.") from None
    if not password:
        raise ValueError("The password file is empty.")
    return password


def b64(data):
    return base64.b64encode(data).decode("ascii")


def encrypt_installer(installer, output, password, name=None):
    modified = datetime.fromtimestamp(installer.stat().st_mtime, timezone.utc)
    if name is None:
        stamp = modified.astimezone(KST).strftime("%Y%m%d-%H%M")
        name = f"{installer.stem}-{stamp}{installer.suffix}"
    if not name or any(character in name for character in "/\\\r\n"):
        raise ValueError("--name must be a file name.")
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise ValueError("The output directory must be empty.")
    (output / "parts").mkdir()

    salt = os.urandom(16)
    key = PBKDF2HMAC(
        algorithm=hashes.SHA256(), length=32, salt=salt, iterations=ITERATIONS
    ).derive(password.encode("utf-8"))
    cipher = AESGCM(key)
    used_ivs = set()

    def new_iv():
        while True:
            iv = os.urandom(12)
            if iv not in used_ivs:
                used_ivs.add(iv)
                return iv

    check_iv = new_iv()
    manifest = {
        "format": 1,
        "name": name,
        "size": 0,
        "sha256": "",
        "date": modified.isoformat(timespec="seconds").replace("+00:00", "Z"),
        "kdf": {
            "name": "PBKDF2", "hash": "SHA-256", "iterations": ITERATIONS,
            "salt": b64(salt),
        },
        "check": {"iv": b64(check_iv), "ct": b64(cipher.encrypt(check_iv, CHECK_TEXT, None))},
        "parts": [],
    }
    digest = hashlib.sha256()
    with installer.open("rb") as source:
        while plain := source.read(PART_SIZE):
            digest.update(plain)
            manifest["size"] += len(plain)
            iv = new_iv()
            ciphertext = cipher.encrypt(iv, plain, None)
            path = f"parts/{hashlib.sha256(ciphertext).hexdigest()[:16]}.bin"
            # Exclusive creation also prevents overwriting a truncated-hash collision.
            with (output / path).open("xb") as part:
                part.write(ciphertext)
            manifest["parts"].append({"path": path, "size": len(ciphertext), "iv": b64(iv)})
    manifest["sha256"] = digest.hexdigest()
    (output / "manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    return manifest


def push_payload(folder):
    def git(*arguments):
        result = subprocess.run(
            ["git", *arguments], cwd=folder, capture_output=True, text=True, errors="replace",
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
        )
        if result.returncode:
            # Show git's own reason (expired login, network); the push URL carries no credentials.
            detail = (result.stderr or result.stdout).strip()[-800:]
            raise ValueError(f"git {arguments[0]} failed ({result.returncode}): {detail}")

    # This is a fresh repository, so its sole commit has no parent.
    git("init")
    git("add", "--", "manifest.json", "parts")
    git("-c", "user.name=DVW download", "-c", "user.email=release@gocue.invalid",
        "-c", "commit.gpgSign=false", "commit", "-m", "Publish DVW download")
    git("push", "--force", REPOSITORY, "HEAD:refs/heads/dvw-download")


def verify_published(manifest, timeout=180):
    deadline = time.monotonic() + timeout

    def fetch(path, limit=None):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("Verification deadline reached.")
        request = Request(
            f"{BASE_URL}{path}?t={time.time_ns()}", headers={"Cache-Control": "no-cache"}
        )
        with urlopen(request, timeout=min(15, remaining)) as response:
            return response.read() if limit is None else response.read(limit)

    while time.monotonic() < deadline:
        try:
            published = json.loads(fetch("manifest.json"))
            # Also compare encryption metadata: republishing the same installer uses
            # a new salt and new parts even when its size and SHA-256 are unchanged.
            if published == manifest:
                if not manifest["parts"]:
                    return True
                first = manifest["parts"][0]
                if len(fetch(first["path"], first["size"] + 1)) == first["size"]:
                    return True
        except (OSError, ValueError, HTTPException):
            pass
        remaining = deadline - time.monotonic()
        if remaining > 0:
            time.sleep(min(5, remaining))
    return False


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    publish = commands.add_parser("publish")
    publish.add_argument("installer", type=Path)
    publish.add_argument("--name")
    publish.add_argument("--password-file", type=Path, default=PASSWORD_FILE)
    publish.add_argument("--dry-run", action="store_true")
    publish.add_argument("--out", type=Path, help="Empty output directory for --dry-run")
    args = parser.parse_args(argv)
    if args.dry_run != (args.out is not None):
        parser.error("--dry-run and --out must be used together")

    try:
        password = read_password(args.password_file)
        if args.dry_run:
            manifest = encrypt_installer(args.installer, args.out, password, args.name)
            result = "dry-run (not uploaded)"
            verified = True
        else:
            with tempfile.TemporaryDirectory(prefix="dvw-download-") as directory:
                folder = Path(directory)
                manifest = encrypt_installer(args.installer, folder, password, args.name)
                push_payload(folder)
                verified = verify_published(manifest)
                result = "passed" if verified else "FAILED after push"
        print(f"{manifest['name']} | {manifest['size']} bytes | "
              f"{len(manifest['parts'])} parts | verification: {result}")
        return 0 if verified else 1
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"dvw_download: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
