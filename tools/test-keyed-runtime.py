#!/usr/bin/env python3
"""Source contracts for coloured, reusable key handling."""

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
runtime = (ROOT / "src" / "keyed_runtime.c").read_text(encoding="utf-8")
makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
converter = (ROOT / "tools" / "wad-flat-playable.py").read_text(encoding="utf-8")

for special, colour in ((26, "BLUE"), (32, "BLUE"), (27, "YELLOW"),
                        (34, "YELLOW"), (28, "RED"), (33, "RED")):
    assert re.search(rf"case {special}:.*?KEY_MASK_{colour}", runtime, re.S)

assert "g_key_mask |= pending_key_mask" in runtime
assert "if (consumed_key != NULL) *consumed_key = FALSE" in runtime
assert "&ignored_consumption" in runtime
assert "billboard_consume_key" not in runtime

for name in ("billboard_init", "billboard_collect_near", "bsp_use_in_front"):
    assert f"-D{name}=keyed_{name}" in makefile

assert "26: KEY_BLUE" in converter and "32: KEY_BLUE" in converter
assert "27: KEY_YELLOW" in converter and "34: KEY_YELLOW" in converter
assert "28: KEY_RED" in converter and "33: KEY_RED" in converter

print("ok    converter and runtime agree on reusable blue/yellow/red keys")
