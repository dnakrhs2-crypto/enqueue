"""Run with python -B tools/test_dvw_download.py; no real credentials or uploads."""

import base64
from contextlib import contextmanager, ExitStack, redirect_stdout
from datetime import datetime, timezone
from functools import partial
import hashlib
from http.client import IncompleteRead
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import io
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
from urllib.parse import urlsplit

from cryptography.exceptions import InvalidTag
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.kdf.pbkdf2 import PBKDF2HMAC
from playwright.sync_api import expect, sync_playwright

sys.dont_write_bytecode = True
import dvw_download as dvw


REPO = Path(__file__).resolve().parents[1]


def run_process(arguments):
    return subprocess.run(
        arguments, capture_output=True, text=True, encoding="utf-8", timeout=120,
        env={**os.environ, "PYTHONIOENCODING": "utf-8"},
        creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
    )


class Payload:
    def __init__(self):
        temporary = tempfile.TemporaryDirectory(prefix="dvw-download-test-")
        unittest.addModuleCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.installer = self.root / "DVW-Setup.exe"
        self.original = os.urandom(40 * 1024 * 1024 + 37)
        self.installer.write_bytes(self.original)
        self.sha256 = hashlib.sha256(self.original).hexdigest()
        modified = datetime(2026, 11, 12, 8, 40, tzinfo=timezone.utc).timestamp()
        os.utime(self.installer, (modified, modified))
        self.password = "시험-" + secrets.token_urlsafe(24)
        self.password_file = self.root / "password.txt"
        self.password_file.write_text("  " + self.password + " \n", encoding="utf-8")
        self.output = self.root / "payload"
        self.result = self.cli("--dry-run", "--out", str(self.output))
        if self.result.returncode:
            raise RuntimeError(self.result.stderr)
        self.manifest = json.loads((self.output / "manifest.json").read_text(encoding="utf-8"))

    def cli(self, *arguments):
        return run_process([
            sys.executable, "-B", str(REPO / "tools/dvw_download.py"), "publish",
            str(self.installer), "--password-file", str(self.password_file), *arguments,
        ])

    def cipher(self, password):
        kdf = self.manifest["kdf"]
        key = PBKDF2HMAC(
            algorithm=hashes.SHA256(), length=32,
            salt=base64.b64decode(kdf["salt"]), iterations=kdf["iterations"],
        ).derive(password.encode("utf-8"))
        return AESGCM(key)


def setUpModule():
    global payload
    payload = Payload()


class PayloadTests(unittest.TestCase):
    def test_01_dry_run_and_python_round_trip(self):
        manifest = payload.manifest
        self.assertEqual(set(manifest), {"format", "name", "size", "sha256", "date", "kdf", "check", "parts"})
        self.assertEqual(manifest["format"], 1)
        self.assertEqual(manifest["name"], "DVW-Setup-20261112-1740.exe")
        self.assertEqual(manifest["date"], "2026-11-12T08:40:00Z")
        self.assertEqual(manifest["size"], len(payload.original))
        self.assertEqual(manifest["sha256"], payload.sha256)
        self.assertEqual(manifest["kdf"], {
            "name": "PBKDF2", "hash": "SHA-256", "iterations": 600000,
            "salt": manifest["kdf"]["salt"],
        })
        self.assertEqual(len(base64.b64decode(manifest["kdf"]["salt"])), 16)
        self.assertEqual(len(manifest["parts"]), 3)
        self.assertEqual({p.name for p in payload.output.iterdir()}, {"manifest.json", "parts"})
        self.assertEqual(len(list((payload.output / "parts").iterdir())), 3)
        self.assertIn("3 parts | verification: dry-run", payload.result.stdout)
        self.assertNotIn(payload.password, payload.result.stdout + payload.result.stderr)

        cipher = payload.cipher(payload.password)
        check = manifest["check"]
        self.assertEqual(set(check), {"iv", "ct"})
        check_iv = base64.b64decode(check["iv"])
        self.assertEqual(len(check_iv), 12)
        self.assertEqual(cipher.decrypt(check_iv, base64.b64decode(check["ct"]), None), b"DVW-DOWNLOAD-V1")
        ivs, paths, plain = {check_iv}, set(), bytearray()
        for part in manifest["parts"]:
            self.assertEqual(set(part), {"path", "size", "iv"})
            ciphertext = (payload.output / part["path"]).read_bytes()
            expected_path = "parts/" + hashlib.sha256(ciphertext).hexdigest()[:16] + ".bin"
            self.assertEqual(part["path"], expected_path)
            self.assertNotIn(part["path"], paths)
            paths.add(part["path"])
            iv = base64.b64decode(part["iv"])
            self.assertEqual(len(iv), 12)
            self.assertNotIn(iv, ivs)
            ivs.add(iv)
            chunk = cipher.decrypt(iv, ciphertext, None)
            self.assertEqual(len(chunk), min(16 * 1024 * 1024, len(payload.original) - len(plain)))
            self.assertEqual(part["size"], len(ciphertext))
            self.assertEqual(len(ciphertext), len(chunk) + 16)
            plain.extend(chunk)
        self.assertTrue(plain == payload.original)

    def test_02_wrong_password_fails_check(self):
        check = payload.manifest["check"]
        with self.assertRaises(InvalidTag):
            payload.cipher("incorrect test password").decrypt(
                base64.b64decode(check["iv"]), base64.b64decode(check["ct"]), None
            )

    def test_03_node_round_trip_and_integrity_errors(self):
        script = payload.root / "verify.cjs"
        script.write_text(r"""
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { createHash } = require('node:crypto');
const core = require(process.argv[2]);
const folder = process.argv[3];
async function main() {
  const manifest = core.validateManifest(JSON.parse(fs.readFileSync(path.join(folder, 'manifest.json'), 'utf8')));
  const password = fs.readFileSync(process.argv[4], 'utf8').trim();
  const key = await core.deriveKey(password, manifest.kdf);
  await core.decryptCheck(key, manifest.check);
  const wrong = await core.deriveKey('incorrect test password', manifest.kdf);
  await assert.rejects(core.decryptCheck(wrong, manifest.check));
  const chunks = [];
  for (const part of manifest.parts) {
    const ciphertext = fs.readFileSync(path.join(folder, part.path));
    chunks.push(await core.decryptPart(key, part, ciphertext));
    await assert.rejects(core.decryptPart(key, part, ciphertext.subarray(1)), /size/);
  }
  const bytes = await core.verifyFile(chunks, manifest);
  assert.equal(createHash('sha256').update(bytes).digest('hex'), manifest.sha256);
  await assert.rejects(core.verifyFile(chunks, { ...manifest, size: manifest.size + 1 }), /size/);
  await assert.rejects(core.verifyFile(chunks, { ...manifest, sha256: '0'.repeat(64) }), /SHA-256/);
  console.log('Node round trip and integrity checks passed');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
""", encoding="utf-8")
        result = run_process([
            "node", str(script), str(REPO / "site/dvw/dl.js"), str(payload.output), str(payload.password_file)
        ])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("checks passed", result.stdout)

    def test_04_name_override_and_missing_or_empty_password(self):
        small = payload.root / "small.exe"
        small.write_bytes(os.urandom(127))
        with patch.object(payload, "installer", small):
            output = payload.root / "named-payload"
            result = payload.cli("--dry-run", "--out", str(output), "--name", "DVW-시험.exe")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads((output / "manifest.json").read_text(encoding="utf-8"))["name"], "DVW-시험.exe")
            for kind, data in [("missing", None), ("empty", b" \r\n"), ("invalid-utf8", b"\xff")]:
                with self.subTest(password_file=kind):
                    password_file = payload.root / (kind + ".txt")
                    if data is not None:
                        password_file.write_bytes(data)
                    target = payload.root / (kind + "-output")
                    result = payload.cli("--dry-run", "--out", str(target), "--password-file", str(password_file))
                    self.assertNotEqual(result.returncode, 0)
                    self.assertFalse(target.exists())
                    self.assertNotIn("Traceback", result.stderr)

    def test_05_publish_isolated_and_only_download_ref(self):
        # No git command or network call is executed in this test.
        with patch.object(dvw.subprocess, "run") as run:
            run.return_value.returncode = 0
            dvw.push_payload(payload.root)
        commands = [call.args[0] for call in run.call_args_list]
        self.assertEqual(commands, [
            ["git", "init"],
            ["git", "add", "--", "manifest.json", "parts"],
            ["git", "-c", "user.name=DVW download", "-c", "user.email=release@gocue.invalid",
             "-c", "commit.gpgSign=false", "commit", "-m", "Publish DVW download"],
            ["git", "push", "--force", "https://github.com/dnakrhs2-crypto/enqueue.git", "HEAD:refs/heads/dvw-download"],
        ])
        self.assertTrue(all(call.kwargs["cwd"] == payload.root for call in run.call_args_list))

        small = payload.root / "publish-test.exe"
        small.write_bytes(os.urandom(127))
        folders = []

        def inspect_payload(folder):
            folders.append(folder)
            self.assertEqual({p.name for p in folder.iterdir()}, {"manifest.json", "parts"})
            self.assertNotEqual(folder, REPO)

        for verified in (True, False):
            with self.subTest(verified=verified), patch.object(dvw, "push_payload", side_effect=inspect_payload), \
                    patch.object(dvw, "verify_published", return_value=verified), redirect_stdout(io.StringIO()) as output:
                result = dvw.main(["publish", str(small), "--password-file", str(payload.password_file)])
                self.assertEqual(result, 0 if verified else 1)
                self.assertIn("verification: " + ("passed" if verified else "FAILED after push"), output.getvalue())
                self.assertNotIn(payload.password, output.getvalue())
                self.assertFalse(folders[-1].exists())

    def test_06_publication_retries_stale_manifest_and_incomplete_part(self):
        manifest = payload.manifest
        first = manifest["parts"][0]
        current = json.dumps(manifest).encode("utf-8")
        # The same plain file can be republished with a new name or encryption salt.
        stale = json.dumps({**manifest, "name": "old.exe"}).encode("utf-8")
        responses = [stale, current, b"truncated", current, (payload.output / first["path"]).read_bytes()]
        with patch.object(dvw, "urlopen", side_effect=[IncompleteRead(b"{", 100)] +
                          [io.BytesIO(data) for data in responses]) as fetch, \
                patch.object(dvw.time, "sleep") as sleep:
            self.assertTrue(dvw.verify_published(manifest))
        self.assertEqual(sleep.call_count, 3)
        for call in fetch.call_args_list:
            self.assertTrue(call.args[0].full_url.startswith(dvw.BASE_URL))
            self.assertRegex(call.args[0].full_url, r"\?t=\d+$")


class LocalHandler(SimpleHTTPRequestHandler):
    def log_message(self, *arguments):
        pass

    def end_headers(self):
        if self.server.cors:
            self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def do_GET(self):
        path = urlsplit(self.path).path
        self.server.requests.append((self.path, self.headers.get("Origin")))
        if path == self.server.missing_part:
            self.send_error(404)
        else:
            super().do_GET()


@contextmanager
def serve(directory, cors=False):
    server = ThreadingHTTPServer(("127.0.0.1", 0), partial(LocalHandler, directory=str(directory)))
    server.cors = cors
    server.requests = []
    server.missing_part = None
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield server
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


class BrowserTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        stack = ExitStack()
        cls.addClassCleanup(stack.close)
        site = payload.root / "site"
        shutil.copytree(REPO / "site", site)
        cls.data_server = stack.enter_context(serve(payload.output, cors=True))
        base = f"http://127.0.0.1:{cls.data_server.server_port}/"
        index = site / "dvw/index.html"
        replaced, count = re.subn(
            rb'(<form\b[^>]*\bid="dl-form"[^>]*\bdata-base=")[^"]*(")',
            lambda match: match[1] + base.encode("ascii") + match[2], index.read_bytes(), count=1,
        )
        if count != 1:
            raise AssertionError("Cannot find the copied download form's data-base")
        index.write_bytes(replaced)
        cls.site_server = stack.enter_context(serve(site))
        cls.origin = f"http://127.0.0.1:{cls.site_server.server_port}"
        playwright = stack.enter_context(sync_playwright())
        cls.browser = playwright.chromium.launch(headless=True)
        stack.callback(cls.browser.close)

    def setUp(self):
        self.data_server.requests.clear()
        self.data_server.missing_part = None
        self.context = self.browser.new_context(accept_downloads=True, timezone_id="America/Los_Angeles")
        self.addCleanup(self.context.close)
        # Fonts and analytics are unrelated to this local, cross-origin download test.
        self.context.route("**/*", lambda route: route.continue_() if
                           urlsplit(route.request.url).hostname == "127.0.0.1" else route.abort())
        self.page = self.context.new_page()
        self.page.goto(self.origin + "/dvw/", wait_until="domcontentloaded")
        expect(self.page.locator("#dl-meta span.nb")).to_have_count(2)
        self.password = self.page.locator("#dl-password")
        self.button = self.page.locator("#dl-button")
        self.error = self.page.locator("#dl-error")
        self.label = self.page.locator("#dl-label")
        self.original_label = self.label.inner_text()

    def part_requests(self):
        return [urlsplit(path).path for path, origin in self.data_server.requests if path.startswith("/parts/")]

    def assert_reset(self):
        expect(self.button).to_be_enabled()
        self.assertIsNone(self.button.get_attribute("aria-busy"))
        expect(self.label).to_have_text(self.original_label)
        self.assertEqual(self.page.locator("#dl-progress").evaluate("node => node.style.getPropertyValue('--p')"), "0")

    def check_download(self, download, filename):
        self.assertEqual(download.suggested_filename, payload.manifest["name"])
        target = payload.root / filename
        download.save_as(target)
        self.assertEqual(hashlib.sha256(target.read_bytes()).hexdigest(), payload.sha256)

    def test_01_metadata_has_size_and_korean_time(self):
        expect(self.page.locator("#dl-meta span.nb")).to_have_text(["40.0 MB", "2026-11-12 17:40"])
        self.assertNotEqual(self.site_server.server_port, self.data_server.server_port)
        manifest_requests = [(path, origin) for path, origin in self.data_server.requests if path.startswith("/manifest.json")]
        self.assertTrue(manifest_requests)
        self.assertTrue(all("?t=" in path and origin == self.origin for path, origin in manifest_requests))

    def test_02_empty_password_and_input_clear(self):
        self.button.click()
        expect(self.error).to_have_text("비밀번호를 입력하세요.")
        expect(self.password).to_have_attribute("aria-invalid", "true")
        expect(self.password).to_be_focused()
        self.password.fill("next input")
        expect(self.error).to_be_empty()
        self.assertIsNone(self.password.get_attribute("aria-invalid"))
        self.assertEqual(self.part_requests(), [])

    def test_03_wrong_password_focus_selection_and_reset(self):
        self.password.fill("incorrect test password")
        self.button.click()
        expect(self.error).to_have_text("비밀번호가 틀렸습니다.")
        expect(self.password).to_have_attribute("aria-invalid", "true")
        expect(self.password).to_be_focused()
        self.assertEqual(self.password.evaluate("node => [node.selectionStart, node.selectionEnd]"), [0, 23])
        self.assert_reset()
        self.assertEqual(self.part_requests(), [])

    def test_04_download_progress_and_password_checked_cache(self):
        self.page.evaluate("""() => {
          window.downloadStates = [];
          const label = document.getElementById('dl-label');
          new MutationObserver(() => {
            if (label.textContent.startsWith('받는 중 ')) {
              const button = document.getElementById('dl-button');
              window.downloadStates.push({
                text: label.textContent, busy: button.getAttribute('aria-busy'), disabled: button.disabled,
                progress: Number(document.getElementById('dl-progress').style.getPropertyValue('--p'))
              });
            }
          }).observe(label, { childList: true, characterData: true, subtree: true });
        }""")
        downloads = []
        self.page.on("download", lambda download: downloads.append(download))
        self.password.fill(payload.password)
        with self.page.expect_download(timeout=60000) as event:
            self.button.click()
        self.check_download(event.value, "downloaded.exe")
        self.assert_reset()
        expect(self.error).to_be_empty()
        states = self.page.evaluate("window.downloadStates")
        self.assertTrue(any(0 < state["progress"] < 1 for state in states))
        self.assertTrue(all(state["busy"] == "true" and state["disabled"] for state in states))
        expected_paths = ["/" + part["path"] for part in payload.manifest["parts"]]
        self.assertEqual(self.part_requests(), expected_paths)

        self.password.fill("incorrect test password")
        self.button.click()
        expect(self.error).to_have_text("비밀번호가 틀렸습니다.")
        self.assertEqual(len(downloads), 1)
        self.assert_reset()
        self.password.fill(payload.password)
        with self.page.expect_download(timeout=60000) as event:
            self.button.click()
        self.check_download(event.value, "downloaded-again.exe")
        self.assertEqual(self.part_requests(), expected_paths)
        manifest_requests = [path for path, origin in self.data_server.requests if path.startswith("/manifest.json?t=")]
        self.assertEqual(len(manifest_requests), 4)  # page load and every submit, including the cached save
        self.assert_reset()

    def test_05_part_404_shows_download_error(self):
        self.data_server.missing_part = "/" + payload.manifest["parts"][1]["path"]
        downloads = []
        self.page.on("download", lambda download: downloads.append(download))
        self.password.fill(payload.password)
        self.button.click()
        expect(self.error).to_have_text("받지 못했습니다. 다시 눌러 주세요.", timeout=60000)
        self.assert_reset()
        self.assertEqual(downloads, [])
        self.assertEqual(self.part_requests(), ["/" + part["path"] for part in payload.manifest["parts"][:2]])

    def test_06_missing_webcrypto(self):
        self.page.evaluate("Object.defineProperty(window, 'crypto', { value: {} })")
        self.password.fill(payload.password)
        self.button.click()
        expect(self.error).to_have_text("이 브라우저에서는 받을 수 없습니다.")
        expect(self.button).to_be_enabled()
        self.assertEqual(self.part_requests(), [])


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
    unittest.main(verbosity=2)
