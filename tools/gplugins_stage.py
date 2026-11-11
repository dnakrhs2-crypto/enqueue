"""Verify and stage the G plugins for the LiveMix installer."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys
import tempfile


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def content_id(files: list[dict]) -> str:
    text = "".join(f"{item['path']}\t{item['size']}\t{item['sha256']}\n"
                   for item in sorted(files, key=lambda item: item["path"]))
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def verify_files(root: Path, files: list[dict], ignored: tuple[str, ...] = ()) -> None:
    expected = {item["path"] for item in files}
    actual = {path.relative_to(root).as_posix() for path in root.rglob("*") if path.is_file()}
    missing = expected - actual
    extra = actual - expected - set(ignored)
    if missing:
        raise ValueError(f"missing files in {root}: {', '.join(sorted(missing))}")
    if extra:
        raise ValueError(f"unlisted files in {root}: {', '.join(sorted(extra))}")
    for item in files:
        path = root / item["path"]
        if path.stat().st_size != item["size"]:
            raise ValueError(f"size mismatch: {path}")
        if sha256(path) != item["sha256"]:
            raise ValueError(f"SHA-256 mismatch: {path}")


def lock_for(payload: dict) -> dict:
    return {"schema": 1, "id": payload["id"], "version": payload["version"],
            "bundles": payload["bundles"], "file_count": len(payload["files"]),
            "total_bytes": sum(item["size"] for item in payload["files"])}


def write_json(path: Path, value: dict) -> None:
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def stage(manifest_path: Path, out: Path, lock: Path | None = None, replace: bool = False) -> dict:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    if manifest.get("schema", manifest.get("schema_version")) != 1:
        raise ValueError("unsupported installer manifest schema")
    if out.exists() and not replace:
        raise ValueError(f"output already exists (use --replace): {out}")
    build = Path(manifest["source_build"])
    sources = []
    files = []
    for bundle in manifest["bundles"]:
        source = build / f"{bundle['target']}_artefacts/Release/VST3"
        name = bundle["bundle"]
        relative_files = [{**item, "path": Path(item["path"]).relative_to(name).as_posix()}
                          for item in bundle["files"]]
        verify_files(source / name, relative_files)
        sources.append((source / name, name))
        files.extend(bundle["files"])
    files.sort(key=lambda item: item["path"])
    payload = {"schema": 1, "id": content_id(files), "version": manifest["version"],
               "source_list_sha256": manifest["source_list_sha256"],
               "bundles": sorted(name for _, name in sources), "files": files}
    # Validate every source before creating anything, then publish a complete sibling directory.
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=out.name + "-", dir=out.parent) as temporary:
        pending = Path(temporary) / "new"
        vst3 = pending / "VST3"
        vst3.mkdir(parents=True)
        for source, name in sources:
            shutil.copytree(source, vst3 / name, copy_function=shutil.copy2)
        write_json(vst3 / "bundled_plugins.json", payload)
        previous = Path(temporary) / "previous"
        if out.exists():
            out.rename(previous)
        try:
            pending.rename(out)
        except OSError:
            if previous.exists():
                previous.rename(out)
            raise
    summary = lock_for(payload)
    if lock is not None:
        write_json(lock, summary)
    print(f"GPLUGINS_STAGED id={payload['id'][:12]} bundles={len(sources)} "
          f"files={summary['file_count']} bytes={summary['total_bytes']}")
    return payload


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--lock", type=Path)
    parser.add_argument("--replace", action="store_true")
    args = parser.parse_args()
    try:
        stage(args.manifest, args.out.resolve(), args.lock, args.replace)
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"GPLUGINS: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
