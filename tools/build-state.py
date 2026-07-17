#!/usr/bin/env python3
"""Track compiler flags that SGDK make dependencies cannot observe."""

import argparse
import hashlib
import json
import os
import re
from pathlib import Path


STATE_VERSION = 1


def read_flags(path: Path) -> str | None:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError, TypeError):
        return None
    if not isinstance(value, dict) or value.get("version") != STATE_VERSION:
        return None
    flags = value.get("extra_flags")
    return flags if isinstance(flags, str) else None


def has_objects(output: Path) -> bool:
    return output.is_dir() and next(output.rglob("*.o"), None) is not None


def dependency_prerequisites(depfile: Path, root: Path) -> list[Path]:
    try:
        rule = depfile.read_text(encoding="utf-8")
    except (OSError, UnicodeError):
        return []
    rule = re.sub(r"\\\r?\n", " ", rule)
    if ":" not in rule:
        return []
    _, prerequisites = rule.split(":", 1)
    result = []
    for token in prerequisites.split():
        if token.endswith(":"):
            continue
        path = Path(token)
        result.append(path if path.is_absolute() else root / path)
    return result


def has_stale_dependencies(output: Path, root: Path | None = None) -> bool:
    if not output.is_dir():
        return False
    root = root or output.parent
    for depfile in output.rglob("*.d"):
        for prerequisite in dependency_prerequisites(depfile, root):
            if not prerequisite.exists():
                return True
    return False


def needs_clean(state: Path, output: Path, flags: str) -> bool:
    if has_stale_dependencies(output):
        return True
    return has_objects(output) and read_flags(state) != flags


def record(state: Path, flags: str) -> None:
    state.parent.mkdir(parents=True, exist_ok=True)
    temp = state.with_suffix(state.suffix + ".tmp")
    temp.write_text(
        json.dumps(
            {"version": STATE_VERSION, "extra_flags": flags},
            indent=2,
            sort_keys=True,
        ) + "\n",
        encoding="utf-8",
    )
    temp.replace(state)


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def rom_is_recorded(state: Path, rom: Path) -> bool:
    if not rom.is_file():
        return False
    try:
        value = json.loads(state.read_text(encoding="utf-8"))
    except (OSError, ValueError, TypeError):
        return False
    return (
        isinstance(value, dict)
        and value.get("version") == STATE_VERSION
        and value.get("sha256") == file_sha256(rom)
    )


def record_rom(state: Path, rom: Path) -> None:
    if not rom.is_file():
        raise FileNotFoundError(f"ROM not found: {rom}")
    state.parent.mkdir(parents=True, exist_ok=True)
    temp = state.with_suffix(state.suffix + ".tmp")
    temp.write_text(
        json.dumps(
            {"version": STATE_VERSION, "sha256": file_sha256(rom)},
            indent=2,
            sort_keys=True,
        ) + "\n",
        encoding="utf-8",
    )
    temp.replace(state)


def seal_link(output: Path, root: Path | None = None) -> None:
    """Restore SGDK's deleted cmd_ dependency so a no-op build stays a no-op."""
    rom = output / "rom.out"
    if not rom.is_file():
        raise FileNotFoundError(f"linked ROM not found: {rom}")
    root = root or output.parent

    def has_source(path: Path) -> bool:
        relative = path.relative_to(output).with_suffix("")
        source_base = root / relative
        return any(source_base.with_suffix(extension).is_file()
                   for extension in (".c", ".s", ".S", ".s80", ".asm", ".res"))

    objects = sorted(path for path in output.rglob("*.o") if has_source(path))
    if not objects:
        raise FileNotFoundError(f"no project objects found below: {output}")
    command = output / "cmd_"
    command.write_text(
        " ".join(path.relative_to(output.parent).as_posix() for path in objects) + "\n",
        encoding="utf-8",
    )
    rom_stat = rom.stat()
    # cmd_ must be at least as new as every object but no newer than rom.out.
    # The successful link guarantees rom.out was written after its objects.
    os.utime(command, ns=(rom_stat.st_atime_ns, rom_stat.st_mtime_ns))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    check = subparsers.add_parser("check")
    check.add_argument("--state", type=Path, required=True)
    check.add_argument("--output", type=Path, required=True)
    check.add_argument("--flags", default="")

    write = subparsers.add_parser("record")
    write.add_argument("--state", type=Path, required=True)
    write.add_argument("--flags", default="")

    seal = subparsers.add_parser("seal-link")
    seal.add_argument("--output", type=Path, required=True)
    seal.add_argument("--root", type=Path)

    check_rom = subparsers.add_parser("check-rom")
    check_rom.add_argument("--state", type=Path, required=True)
    check_rom.add_argument("--rom", type=Path, required=True)

    write_rom = subparsers.add_parser("record-rom")
    write_rom.add_argument("--state", type=Path, required=True)
    write_rom.add_argument("--rom", type=Path, required=True)

    args = parser.parse_args()
    if args.command == "check":
        print("clean" if needs_clean(args.state, args.output, args.flags) else "incremental")
    elif args.command == "record":
        record(args.state, args.flags)
    elif args.command == "seal-link":
        seal_link(args.output, args.root)
    elif args.command == "check-rom":
        print("current" if rom_is_recorded(args.state, args.rom) else "pad")
    else:
        record_rom(args.state, args.rom)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
