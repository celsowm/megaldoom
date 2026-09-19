#!/usr/bin/env python3
"""Report the Mega Drive ROM budget by game resource.

The report is derived from the current build artifacts, so it can be rerun
after any build without maintaining a hand-written size table.

Examples:
    python tools/report-rom-resources.py
    python tools/report-rom-resources.py --json
    python tools/report-rom-resources.py --rom-out build/rom.out --rom-bin build/rom.bin
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Iterable


LEVEL_WINDOW_BASE = 0x280000
LEVEL_WINDOW_BYTES = 0x180000
MAPPER_BANK_BYTES = 0x80000


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="project root (default: repository containing this script)",
    )
    parser.add_argument("--rom-out", type=Path, default=Path("out/rom.out"))
    parser.add_argument("--rom-bin", type=Path, default=Path("out/rom.bin"))
    parser.add_argument(
        "--json",
        action="store_true",
        help="emit machine-readable JSON instead of the Markdown report",
    )
    return parser.parse_args()


def absolute(root: Path, path: Path) -> Path:
    return path if path.is_absolute() else root / path


def find_objdump(root: Path) -> str:
    candidates = [root / ".toolchain/sgdk/bin/objdump.exe", root / ".toolchain/sgdk/bin/objdump"]
    for candidate in candidates:
        if candidate.exists():
            return str(candidate)
    system_objdump = shutil.which("objdump")
    if system_objdump:
        return system_objdump
    raise FileNotFoundError(
        "objdump was not found; install SGDK or pass a toolchain with objdump on PATH"
    )


def run_objdump(objdump: str, mode: str, path: Path) -> list[str]:
    completed = subprocess.run(
        [objdump, mode, str(path)],
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if completed.returncode:
        raise RuntimeError(
            f"objdump {mode} failed for {path}: {completed.stderr.strip()}"
        )
    return completed.stdout.splitlines()


def section_sizes(objdump: str, path: Path) -> dict[str, int]:
    sizes: dict[str, int] = {}
    pattern = re.compile(r"^\s*\d+\s+(\S+)\s+([0-9a-fA-F]{8})\s+")
    for line in run_objdump(objdump, "-h", path):
        match = pattern.match(line)
        if match:
            sizes[match.group(1)] = int(match.group(2), 16)
    return sizes


def symbol_lines(objdump: str, path: Path) -> Iterable[str]:
    return run_objdump(objdump, "-t", path)


def resource_sizes(objdump: str, resources_obj: Path) -> dict[str, int]:
    """Read SGDK's generated *_size absolute symbols.

    In SGDK resource objects the symbol address is the byte size and the
    symbol value field is zero. This is why the first hexadecimal field is
    intentionally used below.
    """

    pattern = re.compile(
        r"^\s*([0-9a-fA-F]+)\s+\S+\s+\*ABS\*\s+[0-9a-fA-F]+\s+(\S+)_size$"
    )
    result: dict[str, int] = {}
    for line in symbol_lines(objdump, resources_obj):
        match = pattern.match(line)
        if match:
            result[match.group(2)] = int(match.group(1), 16)
    return result


def resource_groups(sizes: dict[str, int]) -> dict[str, int]:
    groups: dict[str, int] = {}
    for name, size in sizes.items():
        if name.startswith("frontend_"):
            parts = name.split("_")
            group = f"frontend:{parts[1]}" if len(parts) > 1 else "frontend:other"
        elif name.startswith("sfx_"):
            group = "audio:sfx"
        elif name.endswith("_music"):
            group = "audio:music"
        else:
            group = "resource:other"
        groups[group] = groups.get(group, 0) + size
    return groups


def level_map_sizes(objdump: str, rom_out: Path) -> dict[str, int]:
    pattern = re.compile(
        r"^\s*[0-9a-fA-F]+\s+\S+\s+O\s+\.wallpack(\d+)\s+"
        r"([0-9a-fA-F]+)\s+.*(?:\.hidden\s+)?(e1m\d+)_bsp_\S+$"
    )
    result: dict[str, int] = {}
    for line in symbol_lines(objdump, rom_out):
        match = pattern.match(line)
        if match:
            level = match.group(3).upper()
            result[level] = result.get(level, 0) + int(match.group(2), 16)
    return result


def level_file_sizes(root: Path, prefix: str) -> dict[str, int]:
    result: dict[str, int] = {}
    for path in (root / "src/bsp").glob(f"generated_{prefix}_e1m*.dat"):
        match = re.search(r"(e1m\d+)\.dat$", path.name, re.IGNORECASE)
        if match:
            result[match.group(1).upper()] = path.stat().st_size
    return result


def mib(value: int) -> float:
    return value / (1024 * 1024)


def kib(value: int) -> float:
    return value / 1024


def percent(value: int, total: int) -> float:
    return 100.0 * value / total if total else 0.0


def build_report(root: Path, rom_out: Path, rom_bin: Path, objdump: str) -> dict:
    rom_sections = section_sizes(objdump, rom_out)
    resources_obj = root / "out/res/resources.o"
    scaler_obj = root / "out/src/renderer/generated_wall_scalers.o"

    # The linker places .text at ROM offset zero and .data immediately after
    # it. This is the same resident-end calculation used by check-rom.ps1,
    # expressed from section sizes because section_sizes() intentionally keeps
    # only the size field.
    resident_bytes = rom_sections.get(".text", 0) + rom_sections.get(".data", 0)

    resource_symbols = resource_sizes(objdump, resources_obj)
    resource_groups_by_name = resource_groups(resource_symbols)
    resource_object_sections = section_sizes(objdump, resources_obj)
    resource_object_bytes = sum(
        resource_object_sections.get(section, 0)
        for section in (".rodata", ".rodata_bin", ".rodata_binf")
    )

    scaler_sections = section_sizes(objdump, scaler_obj)
    scaler_bytes = sum(
        scaler_sections.get(section, 0)
        for section in (".text.megaldoom_wall_scalers", ".rodata.megaldoom_wall_scaler_ends")
    )

    # Wall blocks several levels draw, stored once in resident ROM
    # (tools/world_assets.py, SHARED_WALL_BLOCK_BUDGET).
    shared_wall_path = root / "src/bsp/generated_wallpack_shared.dat"
    shared_wall_bytes = shared_wall_path.stat().st_size if shared_wall_path.exists() else 0
    wallpacks = level_file_sizes(root, "wallpack")
    visibility = level_file_sizes(root, "bsp_vis")
    maps = level_map_sizes(objdump, rom_out)
    level_names = sorted(set(wallpacks) | set(visibility) | set(maps), key=lambda x: int(x[3:]))
    levels = []
    for index, level in enumerate(level_names):
        # Packs are padded to whole 512 KB banks (tools/md_banked.ld), so the
        # cartridge space a level takes is its section size, not the window.
        reserved = rom_sections.get(f".wallpack{index}", 0)
        wall_bytes = wallpacks.get(level, 0)
        visibility_bytes = visibility.get(level, 0)
        map_bytes = maps.get(level, 0)
        payload = wall_bytes + visibility_bytes + map_bytes
        levels.append(
            {
                "level": level,
                "wallpacks": wall_bytes,
                "visibility": visibility_bytes,
                "map_data": map_bytes,
                "payload": payload,
                "reserved": reserved,
                "banks": reserved // MAPPER_BANK_BYTES,
                "window_percent": percent(payload, LEVEL_WINDOW_BYTES),
            }
        )

    rom_size = rom_bin.stat().st_size
    level_window_capacity = sum(level["reserved"] for level in levels)
    actual_level_payload = sum(level["payload"] for level in levels)
    frontend_bytes = sum(
        size for name, size in resource_groups_by_name.items() if name.startswith("frontend:")
    )
    audio_bytes = resource_groups_by_name.get("audio:sfx", 0) + resource_groups_by_name.get(
        "audio:music", 0
    )
    resident_other = max(
        0, resident_bytes - frontend_bytes - audio_bytes - scaler_bytes - shared_wall_bytes
    )

    return {
        "rom": {
            "file_bytes": rom_size,
            "file_mib": mib(rom_size),
            "resident_bytes": resident_bytes,
            "resident_mib": mib(resident_bytes),
            "level_window_capacity_bytes": level_window_capacity,
            "level_window_capacity_mib": mib(level_window_capacity),
        },
        "resident_resources": {
            "frontend_graphics": frontend_bytes,
            "audio": audio_bytes,
            "wall_scalers": scaler_bytes,
            "shared_wall_blocks": shared_wall_bytes,
            "engine_and_other": resident_other,
            "resource_object_bytes": resource_object_bytes,
            "groups": resource_groups_by_name,
        },
        "levels": levels,
        "level_totals": {
            "wallpacks": sum(level["wallpacks"] for level in levels),
            "visibility": sum(level["visibility"] for level in levels),
            "map_data": sum(level["map_data"] for level in levels),
            "payload": actual_level_payload,
        },
        "toolchain": {"objdump": objdump},
    }


def format_size(value: int) -> str:
    if value >= 1024 * 1024:
        return f"{mib(value):.2f} MiB"
    return f"{kib(value):.1f} KiB"


def render_markdown(report: dict) -> str:
    rom = report["rom"]
    resources = report["resident_resources"]
    totals = report["level_totals"]
    physical_rom = rom["file_bytes"]

    lines = [
        "# ROM Resource Report",
        "",
        f"ROM image: **{format_size(physical_rom)}** "
        f"({percent(physical_rom, 16 * 1024 * 1024 - 256 * 1024):.1f}% of the effective 15.75 MiB limit)",
        "",
        "## Resident resources",
        "",
        "| Resource | Size | Share of resident image |",
        "|---|---:|---:|",
    ]
    resident = rom["resident_bytes"]
    resident_rows = [
        ("Frontend graphics", resources["frontend_graphics"]),
        ("Audio", resources["audio"]),
        ("Pre-generated wall scalers", resources["wall_scalers"]),
        ("Shared wall blocks (drawn by several levels)", resources["shared_wall_blocks"]),
        ("Engine and other resident data", resources["engine_and_other"]),
    ]
    for name, size in resident_rows:
        lines.append(f"| {name} | {format_size(size)} | {percent(size, resident):.1f}% |")

    lines += [
        "",
        "## Level resources",
        "",
        f"Actual level payload: **{format_size(totals['payload'])}**. "
        f"The levels reserve **{format_size(rom['level_window_capacity_bytes'])}** "
        f"in whole {format_size(MAPPER_BANK_BYTES)} mapper banks "
        f"(each at most the {format_size(LEVEL_WINDOW_BYTES)} window).",
        "",
        "| Level | Wallpacks | Visibility/PVS | BSP/map data | Payload | Banks | Window used |",
        "|---|---:|---:|---:|---:|---:|---:|",
    ]
    for level in report["levels"]:
        lines.append(
            f"| {level['level']} | {format_size(level['wallpacks'])} | "
            f"{format_size(level['visibility'])} | {format_size(level['map_data'])} | "
            f"{format_size(level['payload'])} | {level['banks']} | "
            f"{level['window_percent']:.1f}% |"
        )

    lines += [
        "",
        "### Level totals",
        "",
        f"- Wallpacks: **{format_size(totals['wallpacks'])}**",
        f"- Visibility/PVS: **{format_size(totals['visibility'])}**",
        f"- BSP/map data: **{format_size(totals['map_data'])}**",
        "",
        "## Largest frontend groups",
        "",
        "| Group | Size |",
        "|---|---:|",
    ]
    frontend_groups = [
        (name, size)
        for name, size in resources["groups"].items()
        if name.startswith("frontend:")
    ]
    for name, size in sorted(frontend_groups, key=lambda item: item[1], reverse=True)[:10]:
        lines.append(f"| {name.removeprefix('frontend:')} | {format_size(size)} |")

    lines += [
        "",
        "The report is generated from the current build artifacts; rerun the command after a build.",
    ]
    return "\n".join(lines)


def main() -> int:
    args = parse_args()
    root = args.root.resolve()
    rom_out = absolute(root, args.rom_out)
    rom_bin = absolute(root, args.rom_bin)
    if not rom_out.exists() or not rom_bin.exists():
        print(
            f"Build artifacts not found: {rom_out} and {rom_bin}. Build the ROM first.",
            file=sys.stderr,
        )
        return 2

    try:
        objdump = find_objdump(root)
        report = build_report(root, rom_out, rom_bin, objdump)
    except (FileNotFoundError, RuntimeError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        print(render_markdown(report))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
