#!/usr/bin/env python3
"""Emit a compact perceptual/dither lookup consumed by the PowerShell assets."""

import argparse
import json

import world_palette


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--palette", required=True,
                        help="16 comma-separated RRGGBB colours")
    args = parser.parse_args()
    palette = [tuple(int(value[offset:offset + 2], 16) for offset in (0, 2, 4))
               for value in args.palette.split(",")]
    if len(palette) != 16:
        raise SystemExit("expected exactly 16 palette colours")

    first = []
    second = []
    coverage = []
    for red in range(32):
        for green in range(32):
            for blue in range(32):
                rgb = (red * 255 // 31, green * 255 // 31, blue * 255 // 31)
                a, b, mix = world_palette.best_mix(rgb, palette, True)
                first.append(a)
                second.append(b)
                coverage.append(mix)
    print(json.dumps({"first": first, "second": second, "coverage": coverage},
                     separators=(",", ":")))


if __name__ == "__main__":
    main()
