"""Pin-verified noVNC vendoring; stdlib only, no package scripts or tar extraction.

Default verifies checked-in bytes. --install needs both pinned archives (or --fetch).
--write-assets-manifest intentionally refreshes first-party asset hashes after review.
"""
import argparse
import base64
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import stat
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / "server/admin/console"
PIN_PATH = ROOT / "tools/vendor-novnc.pin.json"
MAX_ARCHIVE = 2 * 1024 * 1024


def sha(data):
    return hashlib.sha256(data).hexdigest()


def safe_relative(value):
    if not isinstance(value, str) or not value or "\\" in value or ":" in value:
        raise ValueError("invalid relative path")
    parts = value.split("/")
    if any(p in ("", ".", "..") for p in parts) or PurePosixPath(value).is_absolute():
        raise ValueError("invalid path components")
    return value


def no_links(path):
    for parent in [path, *path.parents]:
        try:
            info = parent.lstat()
        except FileNotFoundError:
            info = None
        if info is not None and (stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400):
            raise ValueError("symlink or reparse point forbidden")
        if parent == ASSETS:
            break


def unpack(data, prefix):
    if len(data) > MAX_ARCHIVE:
        raise ValueError("archive too large")
    result, seen, total = {}, set(), 0
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as archive:
        for index, member in enumerate(archive):
            if index >= 4096:
                raise ValueError("too many archive entries")
            name = member.name.rstrip("/")
            safe_relative(name)
            if name in seen:
                raise ValueError("duplicate archive entry")
            seen.add(name)
            if name != prefix and not name.startswith(prefix + "/"):
                raise ValueError("unexpected archive root")
            if member.isdir():
                continue
            if not member.isfile() or member.size > 4 * 1024 * 1024:
                raise ValueError("nonregular or oversized archive member")
            total += member.size
            if total > 32 * 1024 * 1024:
                raise ValueError("unpacked archive too large")
            relative = name[len(prefix) + 1:]
            safe_relative(relative)
            result[relative] = archive.extractfile(member).read()
    return result


def selected(name):
    return (name.startswith(("core/", "vendor/", "docs/LICENSE.")) or
            name in {"AUTHORS", "LICENSE.txt", "README.md", "package.json"})


def pinned_bytes(path, source, fetch):
    if path:
        data = Path(path).read_bytes()
    elif fetch:
        # Pin file contains only reviewed primary HTTPS URLs; never execute fetched code.
        if not source["url"].startswith(("https://registry.npmjs.org/", "https://codeload.github.com/")):
            raise ValueError("unapproved archive source")
        with urllib.request.urlopen(source["url"], timeout=60) as response:
            if response.url != source["url"]:
                raise ValueError("archive redirect forbidden")
            data = response.read(MAX_ARCHIVE + 1)
    else:
        raise ValueError("provide pinned archives or --fetch")
    if len(data) > MAX_ARCHIVE or sha(data) != source["sha256"]:
        raise ValueError("pinned archive hash mismatch")
    if "integrity" in source:
        actual = "sha512-" + base64.b64encode(hashlib.sha512(data).digest()).decode("ascii")
        if actual != source["integrity"]:
            raise ValueError("npm integrity mismatch")
    return data


def records(base):
    no_links(base)
    result = []
    for folder, dirs, files in os.walk(base, followlinks=False):
        for child in [*dirs, *files]:
            no_links(Path(folder) / child)
        for name in files:
            path = Path(folder) / name
            if not path.is_file():
                raise ValueError("nonregular asset")
            relative = safe_relative(path.relative_to(base).as_posix())
            data = path.read_bytes()
            result.append({"path": relative, "bytes": len(data), "sha256": sha(data)})
    return sorted(result, key=lambda row: row["path"])


def write_json(path, value, immutable=False):
    no_links(path)
    data = (json.dumps(value, indent=2, ensure_ascii=False) + "\n").encode("utf-8")
    if immutable and path.exists():
        existing = path.read_bytes()
        if existing not in (data, data.replace(b"\n", b"\r\n")):
            raise ValueError("existing vendor manifest differs")
        return  # Preserve identical manifest bytes, including checkout line endings.
    path.write_bytes(data)


def verify_rows(base, rows):
    if not isinstance(rows, list) or len(rows) > 4096:
        raise ValueError("invalid file manifest")
    seen = set()
    for row in rows:
        name = safe_relative(row["path"])
        if name in seen:
            raise ValueError("duplicate manifest path")
        seen.add(name)
        path = base / name
        no_links(path)
        data = path.read_bytes()
        if len(data) != row["bytes"] or sha(data) != row["sha256"]:
            raise ValueError("manifest content mismatch: " + name)
    return seen


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--install", action="store_true")
    parser.add_argument("--fetch", action="store_true")
    parser.add_argument("--npm-archive")
    parser.add_argument("--github-archive")
    parser.add_argument("--write-assets-manifest", action="store_true")
    args = parser.parse_args()
    pin = json.loads(PIN_PATH.read_text(encoding="utf-8"))
    base = ASSETS / "vendor/novnc"
    version = base / pin["version"]
    manifest_path = base / "manifest.json"
    if args.install:
        npm = unpack(pinned_bytes(args.npm_archive, pin["npm"], args.fetch), "package")
        github = unpack(pinned_bytes(args.github_archive, pin["github"], args.fetch), "noVNC-" + pin["gitCommit"])
        chosen = {name: data for name, data in npm.items() if selected(name)}
        if len(chosen) != 64:
            raise ValueError("unexpected pinned file inventory")
        for name, data in chosen.items():
            if github.get(name) != data:
                raise ValueError("npm/tag source mismatch")
        # Validate existing destinations before writing; never overwrite user changes.
        for name, data in chosen.items():
            path = version / name
            no_links(path)
            if path.exists() and path.read_bytes() != data:
                raise ValueError("existing vendor differs: " + name)
        for name, data in chosen.items():
            path = version / name
            path.parent.mkdir(parents=True, exist_ok=True)
            if not path.exists():
                with path.open("xb") as output:
                    output.write(data)
        write_json(manifest_path, {"schema": 1, "package": pin["package"], "version": pin["version"],
                   "gitCommit": pin["gitCommit"], "sources": {"release": pin["release"], "npm": pin["npm"], "github": pin["github"]},
                   "files": [{**row, "path": pin["version"] + "/" + row["path"]} for row in records(version)]}, immutable=True)
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if (manifest.get("schema"), manifest.get("package"), manifest.get("version"), manifest.get("gitCommit")) != (1, pin["package"], pin["version"], pin["gitCommit"]):
        raise ValueError("vendor identity mismatch")
    files = verify_rows(base, manifest["files"])
    if files != {pin["version"] + "/" + row["path"] for row in records(version)}:
        raise ValueError("unlisted vendor file")
    assets_path = ASSETS / "assets-manifest.json"
    if args.write_assets_manifest:
        write_json(assets_path, {"schema": 1, "product": "multitap-browser-console", "files": [row for row in records(ASSETS) if row["path"] != "assets-manifest.json"]})
    assets = json.loads(assets_path.read_text(encoding="utf-8"))
    listed = verify_rows(ASSETS, assets["files"])
    if listed != {row["path"] for row in records(ASSETS) if row["path"] != "assets-manifest.json"}:
        raise ValueError("unlisted console asset")
    print(json.dumps({"vendorFiles": len(files), "consoleFiles": len(listed), "commit": pin["gitCommit"], "assetsManifestSha256": sha(assets_path.read_bytes())}))


if __name__ == "__main__":
    main()
