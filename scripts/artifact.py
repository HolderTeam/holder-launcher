"""Package/verify exact launcher build outputs using only the Python standard library."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil


def digest(path):
    with path.open("rb") as stream:
        result = hashlib.sha256()
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
        return result.hexdigest()


def files(root):
    result = {}
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            raise ValueError(f"Symlink is not an artifact file: {path}")
        if path.is_file():
            result[path.relative_to(root).as_posix()] = digest(path)
    return result


def safe_name(name):
    path = PurePosixPath(name)
    if not name or path.is_absolute() or ".." in path.parts or "\\" in name or ":" in name or "\n" in name or "\r" in name:
        raise ValueError(f"Invalid artifact path: {name!r}")
    return name


def verify(root):
    actual = files(root)
    manifest = (root / "manifest.txt").read_text(encoding="utf-8").splitlines()
    if manifest != sorted(actual):
        raise ValueError("Manifest does not match the artifact's exact file inventory")
    checksums = {}
    for line in (root / "SHA256SUMS").read_text(encoding="utf-8").splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  (.+)", line)
        if not match or match[2] in checksums:
            raise ValueError("Malformed/duplicate checksum record")
        checksums[safe_name(match[2])] = match[1]
    if checksums != {name: sha for name, sha in actual.items() if name != "SHA256SUMS"}:
        raise ValueError("Artifact checksums do not match")
    info = json.loads((root / "build-info.json").read_text(encoding="utf-8"))
    payload = {name: sha for name, sha in actual.items() if name not in ("build-info.json", "manifest.txt", "SHA256SUMS")}
    if info["schema_version"] != 1 or info["files"] != payload:
        raise ValueError("Build metadata does not match payload")
    if payload.get(info["binary"]) != info["binary_sha256"]:
        raise ValueError("Binary does not match its captured build record")
    return info


def create(binary, build_record, symbols, output, allow_dirty=False):
    info = json.loads(build_record.read_text(encoding="utf-8"))
    if digest(binary) != info["binary_sha256"]:
        raise ValueError("Binary changed since linking; rebuild before packaging")
    source = info["source"]
    known = re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", source["commit"])
    if not allow_dirty and (source["dirty"] is not False or not known):
        raise ValueError("Candidate artifacts require a clean, identified source commit; --allow-dirty is for local inspection only")
    if not symbols.exists():
        raise ValueError(f"Missing debug symbols: {symbols}; use RelWithDebInfo")
    if binary.name in ("build-info.json", "manifest.txt", "SHA256SUMS") or binary.name == symbols.name:
        raise ValueError("Conflicting artifact filenames")
    if symbols.is_symlink() or binary.is_symlink():
        raise ValueError("Build outputs must not be symlinks")
    if symbols.is_dir():
        if not files(symbols):
            raise ValueError("Debug-symbol directory is empty")
    elif symbols.stat().st_size == 0:
        raise ValueError("Debug-symbol file is empty")
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise ValueError("Output directory must be empty; existing artifacts are never overwritten")
    shutil.copy2(binary, output / binary.name)
    if symbols.is_dir():
        shutil.copytree(symbols, output / symbols.name)
    else:
        shutil.copy2(symbols, output / symbols.name)
    info.update(schema_version=1, binary=binary.name, files=files(output))
    # Record only public run identifiers, never the full environment or tokens.
    info["ci"] = {name: os.environ[name] for name in (
        "GITHUB_SERVER_URL", "GITHUB_REPOSITORY", "GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT",
        "GITHUB_WORKFLOW", "GITHUB_REF", "GITHUB_SHA") if name in os.environ}
    (output / "build-info.json").write_text(json.dumps(info, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    inventory = sorted([*files(output), "manifest.txt", "SHA256SUMS"])
    (output / "manifest.txt").write_text("\n".join(inventory) + "\n", encoding="utf-8")
    hashes = files(output)
    (output / "SHA256SUMS").write_text("".join(f"{sha}  {name}\n" for name, sha in hashes.items()), encoding="utf-8")
    verify(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    pack = commands.add_parser("create")
    for flag in ("binary", "build-record", "symbols", "output"):
        pack.add_argument("--" + flag, type=Path, required=True)
    pack.add_argument("--allow-dirty", action="store_true")
    check = commands.add_parser("verify")
    check.add_argument("directory", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "verify":
            verify(args.directory)
        else:
            create(args.binary, args.build_record, args.symbols, args.output, args.allow_dirty)
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f"Artifact error: {error}\n")


if __name__ == "__main__":
    main()
