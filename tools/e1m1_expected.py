"""Single source of truth for E1M1's map-structure regression constants.

These are facts about ONE fixed input (the pinned WAD in tools/wad_source.py)
run through ONE fixed, independently-tested extraction pipeline (doom_map.py).
They are not independent design choices -- they are what that WAD produces --
so there is no value in five different test files each re-typing their own
copy "just in case." There is real cost to it: when the pinned WAD was last
corrected (see tools/wad_source.py), every one of those copies had to be
found and hand-updated, and a missed one would have silently passed a test
checking the wrong number instead of catching a real regression.

If the pinned WAD ever changes again, update this file (ideally by rerunning
the introspection snippet in each constant's neighbouring test, or by reading
the freshly generated src/bsp/generated_e1m1_map.c header comment) and every
consumer picks it up automatically.
"""

# BspMapData's numeric row, in the exact order tools/bsp_emit.py emits it.
E1M1_ROOT_NODE_INDEX = 235
E1M1_SEG_COUNT = 386
E1M1_VERTEX_COUNT = 467
E1M1_SUBSECTOR_COUNT = 237
E1M1_NODE_COUNT = 236
E1M1_DOOR_GROUP_COUNT = 4
E1M1_THING_COUNT = 138
E1M1_SECTOR_COUNT = 85
E1M1_SECRET_SECTOR_COUNT = 3

E1M1_HEADER_ROW = "%du, %du, %du, %du, %du, %du, %du, %du, %du," % (
    E1M1_ROOT_NODE_INDEX, E1M1_SEG_COUNT, E1M1_VERTEX_COUNT,
    E1M1_SUBSECTOR_COUNT, E1M1_NODE_COUNT, E1M1_DOOR_GROUP_COUNT,
    E1M1_THING_COUNT, E1M1_SECTOR_COUNT, E1M1_SECRET_SECTOR_COUNT)

# Seg type breakdown (doom_map.SEG_WALL/SEG_DOOR/SEG_EXIT/SEG_TRIGGER/
# SEG_WINDOW/SEG_SKY_WALL), summing to E1M1_SEG_COUNT. This WAD's E1M1 has no
# remote-trigger door lines, so SEG_TRIGGER is absent rather than zero (see
# test-sector-map.py).
E1M1_WALL_SEG_COUNT = 315
E1M1_DOOR_SEG_COUNT = 16  # E1M1_DOOR_GROUP_COUNT * 4 faces per group
E1M1_EXIT_SEG_COUNT = 1

# Doom's three window structures on E1M1 -- the pair looking onto the nukage
# courtyard, the three in the exit room, and the east one -- reach the ROM as
# SEG_WINDOW. These 15 segs come from exactly 7 linedefs (26, 29, 117, 119,
# 121, 275, 276); the BSP splits some of them. They were SEG_WALL before, and
# the reclassification is required to be geometry-neutral, which
# test-sector-map.py checks rather than assumes.
E1M1_WINDOW_SEG_COUNT = 15
E1M1_WINDOW_LINEDEF_COUNT = 7

# One-sided map-edge walls in front of an F_SKY1 sector (sectors 5, 13, 20,
# 22, 28, 30, 47) reach the ROM as SEG_SKY_WALL instead of SEG_WALL, so the
# renderer can leave the sky visible above them (this engine has no per-wall
# height, so a textured full-height wall here would otherwise hide almost all
# of the sky). 39 segs come from exactly 35 linedefs; the BSP splits some of
# them. Reclassified from SEG_WALL, same geometry-neutral proof as windows.
E1M1_SKY_WALL_SEG_COUNT = 39
E1M1_SKY_WALL_LINEDEF_COUNT = 35

# doom_map.load_map(..., apply_recipes=True) certificate/BFS result.
E1M1_EXIT_SEG_INDEX = 376
E1M1_CERTIFICATE_STATES = 164

# The "start-room-computer-bank" curated material transfer (see
# flat_map_recipes.FLAT_MATERIAL_TRANSFER_RECIPES["E1M1"]) matches by
# geometry, not by linedef id, so which ids these are is itself a fact about
# the pinned WAD's linedef numbering, not a design choice.
E1M1_CURATED_SOURCE_LINEDEF = 53
E1M1_CURATED_TARGET_LINEDEFS = (42, 43, 44, 55, 56)

# tools/wall_bake_preview.py's E1M1-only preview total: one packed byte per
# pair column per row, for every wall material plus every distinct door
# texture actually used on an E1M1 SEG_DOOR (4, not the campaign-wide 5 --
# generated_assets.h ships the union over the whole campaign, so it is
# unaffected). Preview-only bookkeeping, not a shipped ROM table size.
E1M1_PREVIEW_PACKED_PAIR_BYTES = 851968
