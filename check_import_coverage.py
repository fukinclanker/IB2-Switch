#!/usr/bin/env python3
"""Verify that the wrapper table/module exports cover the target ABI."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

from audit_target import read_targets
from elf import ElfError, parse_elf


def table_names(source: str) -> set[str]:
    names = set(re.findall(r'MAP_TO\("([^"]+)"', source))
    names.update(re.findall(r"\bMAP\(([A-Za-z_][A-Za-z0-9_]*)\)", source))
    names.discard("name")
    return names


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", type=Path, help="APK or extracted/runtime directory")
    parser.add_argument(
        "--imports-source",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "source" / "imports.c",
    )
    args = parser.parse_args()

    try:
        libraries = read_targets(args.target)
        game = parse_elf(libraries["libib3.so"])
        mapped = table_names(args.imports_source.read_text(encoding="utf-8"))
    except (OSError, KeyError, ElfError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    # libib3.so is self-contained: every undefined symbol must come from the
    # wrapper's import table (its own exports are resolved by the loader).
    targets = (("libib3.so", set(game.undefined), set(game.exports)),)
    failed = False
    for name, required, cross_exports in targets:
        from_table = required & mapped
        from_module = required & cross_exports
        missing = sorted(required - mapped - cross_exports)
        print(
            f"{name}: required={len(required)} table={len(from_table)} "
            f"cross-module={len(from_module)} missing={len(missing)}"
        )
        for symbol in missing:
            print(f"  MISSING {symbol}")
        failed |= bool(missing)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())

