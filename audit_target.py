#!/usr/bin/env python3
"""Audit the native ABI of an Infinity Blade III APK or extracted directory."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import sys
import zipfile

from elf import ElfError, parse_elf


LIBRARIES = {
    "libib3.so": "lib/arm64-v8a/libib3.so",
}
KNOWN_SHA256 = {
    "libib3.so": "94ca83a3713bd65bef20545e5b72c2fac12fd747425eae9c24fda5aef6cc9340",
}


def read_targets(path: Path) -> dict[str, bytes]:
    if path.is_dir():
        result = {}
        for name, member in LIBRARIES.items():
            candidates = (path / member, path / name)
            found = next((candidate for candidate in candidates if candidate.is_file()), None)
            if found is None:
                raise FileNotFoundError(f"missing {member} (or {name}) under {path}")
            result[name] = found.read_bytes()
        return result
    with zipfile.ZipFile(path) as apk:
        return {name: apk.read(member) for name, member in LIBRARIES.items()}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", type=Path, help="APK or extracted/runtime directory")
    parser.add_argument("--list-imports", action="store_true", help="print every undefined symbol")
    args = parser.parse_args()

    try:
        libraries = read_targets(args.target)
        infos = {name: parse_elf(data) for name, data in libraries.items()}
    except (OSError, KeyError, zipfile.BadZipFile, ElfError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    all_known = True
    for name in LIBRARIES:
        data, info = libraries[name], infos[name]
        digest = hashlib.sha256(data).hexdigest()
        known = digest == KNOWN_SHA256[name]
        all_known &= known
        print(f"{name}")
        print(f"  format: ELF{info.bits}, machine={info.machine}")
        print(f"  size: {len(data)}")
        print(f"  sha256: {digest} ({'known target' if known else 'UNRECOGNIZED'})")
        print(f"  needed: {', '.join(info.needed) or '(none)'}")
        print(f"  exports: {len(info.exports)}")
        print(f"  undefined imports: {len(info.undefined)}")
        if args.list_imports:
            for symbol in info.undefined:
                print(f"    {symbol}")

    game = infos["libib3.so"]
    for entry in ("ANativeActivity_onCreate", "android_main"):
        print(f"entry {entry}: {'present' if entry in game.exports else 'MISSING'}")
    print(f"target verdict: {'supported fingerprint' if all_known else 'audit required before porting'}")
    return 0 if all_known else 1


if __name__ == "__main__":
    raise SystemExit(main())

