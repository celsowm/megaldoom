#!/usr/bin/env python3
"""Resolve a linker symbol's address from SGDK's out/symbol.txt (nm -n -l output).

Used to point BlastEm's --md-mailbox / --md-perf-mailbox at a work-RAM global
by name, instead of hardcoding an address that shifts every time the linker
reflows .bss (see src/debug_checkpoint.c for the symbols this is meant for:
g_debug_checkpoint_state, g_debug_perf_mailbox).
"""
import argparse
import re
import sys

LINE_RE = re.compile(r"^([0-9A-Fa-f]+)\s+[A-Za-z]\s+(\S+)")


def resolve(symbol_path, name):
    with open(symbol_path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = LINE_RE.match(line)
            if m and m.group(2) == name:
                # SGDK's linker script emits work-RAM (.bss/.data) symbols with a
                # nonzero top byte (e.g. 0xE0FF987C) that isn't part of the real
                # 24-bit 68000 address bus; the M68K only decodes the low 24
                # bits, and BlastEm's mailbox reader expects a plain
                # 0xFF0000-0xFFFFFF work-RAM address.
                return int(m.group(1), 16) & 0xFFFFFF
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("symbol_file", help="path to out/symbol.txt")
    parser.add_argument("name", help="symbol name to resolve")
    parser.add_argument("--bytes", type=int, default=None,
                         help="if given, print ADDRESS:BYTES instead of just ADDRESS")
    args = parser.parse_args()

    address = resolve(args.symbol_file, args.name)
    if address is None:
        print(f"symbol not found: {args.name}", file=sys.stderr)
        sys.exit(1)

    if args.bytes is not None:
        print(f"{address:X}:{args.bytes}")
    else:
        print(f"{address:X}")


if __name__ == "__main__":
    main()
