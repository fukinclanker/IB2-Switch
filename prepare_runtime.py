#!/usr/bin/env python3
"""Create a private Switch runtime from a user-owned IB3 APK and IPA.

libib3.so (from the APK) is an iOS-compatibility runtime; the game itself is
the original iOS release of Infinity Blade III, taken from the IPA. The layout
produced here mirrors what the Android launcher installs:

    libib3.so
    game/Payload/SwordGame.app/...      (contents of the IPA's app bundle)
    SaveData/
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import sys
import tempfile
import zipfile

from elf import ElfError, parse_elf


LIBRARY = (
    "libib3.so",
    "lib/arm64-v8a/libib3.so",
    4_133_456,
    "94ca83a3713bd65bef20545e5b72c2fac12fd747425eae9c24fda5aef6cc9340",
)
APP_PREFIX = "Payload/SwordGame.app/"
SENTINEL = APP_PREFIX + "CookedIPhone/Engine.xxx"


def hash_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def hash_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def safe_bundle_path(member: str) -> Path | None:
    """Map an IPA member to game/<member>, or None if it is not app content."""
    if not member.startswith(APP_PREFIX) or member.endswith("/"):
        return None
    posix = PurePosixPath(member)
    if any(part in ("", ".", "..") for part in posix.parts):
        raise ValueError(f"unsafe IPA member path: {member}")
    return Path("game", *posix.parts)


def copy_member(archive: zipfile.ZipFile, member: str, destination: Path) -> int:
    size = 0
    destination.parent.mkdir(parents=True, exist_ok=True)
    with archive.open(member) as source, destination.open("wb") as output:
        while True:
            chunk = source.read(1024 * 1024)
            if not chunk:
                break
            output.write(chunk)
            size += len(chunk)
    return size


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("apk", type=Path, help="IB3 Android APK (provides libib3.so)")
    parser.add_argument("--ipa", type=Path, required=True,
                        help="your own Infinity Blade III iOS IPA (v1.4.4)")
    parser.add_argument("--output", type=Path, required=True, help="new runtime directory")
    parser.add_argument("--nro", type=Path, help="optional built infinityblade3_nx.nro")
    args = parser.parse_args()

    apk_path = args.apk.resolve()
    ipa_path = args.ipa.resolve()
    output = args.output.resolve()
    for label, path in (("APK", apk_path), ("IPA", ipa_path)):
        if not path.is_file():
            parser.error(f"{label} does not exist: {path}")
    if output.exists():
        parser.error(f"refusing to overwrite existing output: {output}")
    if args.nro and not args.nro.is_file():
        parser.error(f"NRO does not exist: {args.nro}")

    output.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=f".{output.name}-", dir=output.parent))
    try:
        out_name, member, expected_size, expected_hash = LIBRARY
        with zipfile.ZipFile(apk_path) as apk:
            if member not in apk.namelist():
                raise ValueError(f"APK is missing {member}")
            data = apk.read(member)
        actual_hash = hash_bytes(data)
        if len(data) != expected_size or actual_hash != expected_hash:
            raise ValueError(f"unsupported {out_name}: size={len(data)} sha256={actual_hash}")
        info = parse_elf(data)
        if info.bits != 64 or info.machine != 183:
            raise ValueError(f"{out_name} is not an AArch64 ELF")
        (staging / out_name).write_bytes(data)

        file_count = 0
        byte_count = 0
        with zipfile.ZipFile(ipa_path) as ipa:
            names = set(ipa.namelist())
            if SENTINEL not in names:
                raise ValueError(f"IPA does not look like Infinity Blade III: missing {SENTINEL}")
            for entry in sorted(names):
                relative = safe_bundle_path(entry)
                if relative is None:
                    continue
                byte_count += copy_member(ipa, entry, staging / relative)
                file_count += 1
        if file_count == 0:
            raise ValueError("IPA contains no Payload/SwordGame.app content")

        (staging / "SaveData").mkdir()
        if args.nro:
            shutil.copyfile(args.nro.resolve(), staging / "infinityblade3_nx.nro")

        manifest = {
            "format": 2,
            "target": "Infinity Blade III iOS runtime (libib3.so, arm64-v8a) + iOS IPA",
            "source_apk": {"filename": apk_path.name, "size": apk_path.stat().st_size,
                           "sha256": hash_file(apk_path)},
            "source_ipa": {"filename": ipa_path.name, "size": ipa_path.stat().st_size},
            "library": {"name": out_name, "apk_member": member, "size": len(data),
                        "sha256": actual_hash, "needed": list(info.needed),
                        "undefined_imports": len(info.undefined)},
            "game": {"files": file_count, "bytes": byte_count},
            "nro": ({"filename": "infinityblade3_nx.nro", "size": args.nro.stat().st_size,
                     "sha256": hash_file(args.nro)} if args.nro else None),
        }
        (staging / "runtime-manifest.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        os.replace(staging, output)
        print(f"Prepared {output}")
        print(f"Validated libib3.so; extracted {file_count} game files ({byte_count} bytes)")
        return 0
    except (OSError, ValueError, KeyError, zipfile.BadZipFile, ElfError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    finally:
        if staging.exists():
            shutil.rmtree(staging)


if __name__ == "__main__":
    raise SystemExit(main())
