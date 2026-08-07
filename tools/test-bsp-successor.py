#!/usr/bin/env python3
"""Contracts for successor-based skipping of solid BSP samples."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RENDERER = (ROOT / "src/bsp/bsp_render.c").read_text()


def find_next(parent, sample):
    root = sample
    while parent[root] != root:
        root = parent[root]
    while sample != root:
        next_sample = parent[sample]
        parent[sample] = root
        sample = next_sample
    return root


def close(parent, sample):
    parent[sample] = find_next(parent, sample + 1)


def main():
    assert "g_next_open[BSP_SAMPLE_COLS + 1]" in RENDERER
    assert "static u16 find_next_open(u16 sample)" in RENDERER
    assert "g_next_open[sample] = (u8)root;" in RENDERER
    assert "g_next_open[sample] = (u8)find_next_open((u16)(sample + 1));" in RENDERER
    assert "u16 sample = find_next_open(first_sample);" in RENDERER
    assert "while (sample <= last_sample)" in RENDERER
    assert "g_next_open[BSP_SAMPLE_COLS] = BSP_SAMPLE_COLS;" in RENDERER
    assert "g_sample_solid" not in RENDERER

    # Closed runs collapse to their first open successor, independent of close
    # order. Open samples (including moving-door overlays) remain visitable.
    parent = list(range(9))
    for sample in (3, 2, 5, 4):
        close(parent, sample)
    assert find_next(parent, 0) == 0
    assert find_next(parent, 1) == 1
    assert find_next(parent, 2) == 6
    assert find_next(parent, 3) == 6
    assert find_next(parent, 4) == 6
    assert find_next(parent, 5) == 6
    assert find_next(parent, 6) == 6
    close(parent, 6)
    close(parent, 7)
    assert find_next(parent, 2) == 8  # one-past-view sentinel

    print("ok    BSP successor: solid runs skip to the next open sample")


if __name__ == "__main__":
    main()
