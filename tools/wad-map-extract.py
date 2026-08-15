#!/usr/bin/env python3
"""Parse, flatten, certify and atomically emit a Doom map for MegalDoom.

Doom's prebuilt VERTEXES/SEGS/SSECTORS/NODES map almost 1:1 onto our Bsp*
structs (even the 0x8000 subsector-leaf bit is identical), so we convert them
directly -- no on-device node builder needed.

This is the CLI entry point only: it wires together wad_reader.py (raw WAD
lump access), doom_map.py (parse + flatten + certify one map), world_assets.py
(bake wall/flat textures and PAL3) and bsp_emit.py (render the result as C),
then prints a report. See those modules for the actual logic.

Usage: python tools/wad-map-extract.py [--map E1M1] [--wad DOOM1.WAD]
"""

import argparse
import os

import bsp_emit
import doom_map
import world_assets
from wad_reader import WadFile

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_WAD = os.path.join(PROJECT_ROOT, "DOOM1.WAD")
# Generated headers live beside the module that consumes them (src/<group>/).
DEFAULT_ASSET_OUT = os.path.join(PROJECT_ROOT, "src", "bsp", "generated_assets.h")

bsp_emit.set_wall_tex_dim(world_assets.WALL_TEX_DIM)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wad", default=DEFAULT_WAD)
    ap.add_argument("--map", default="E1M1")
    ap.add_argument("--out", default=None)
    ap.add_argument("--assets-out", default=DEFAULT_ASSET_OUT)
    ap.add_argument("--certify-only", action="store_true",
                    help="parse/classify/certify and report without emitting files")
    args = ap.parse_args()

    wad = WadFile(args.wad)
    mapn = args.map.upper()
    out_path = args.out or os.path.join(
        PROJECT_ROOT, "src", "generated_%s_map.c" % mapn.lower()
    )

    map_data = doom_map.load_map(wad, mapn)

    if args.certify_only:
        print("Map %s flat progression certified" % mapn)
        print("  doors    : %d groups, faces=%s" %
              (map_data.next_door_group, dict(sorted(map_data.door_face_counts.items()))))
        print("  keys     : available=0x%02X required=0x%02X reached=%s" %
              (map_data.certificate["available_keys"], map_data.required_key_mask,
               map_data.certificate["reached_masks"]))
        print("  exit     : seg %d with key mask=0x%02X after %d states" %
              (map_data.certificate["exit_index"], map_data.certificate["key_mask"],
               map_data.certificate["states"]))
        print("  textures : %d door faces still require fallback" %
              map_data.fallback_door_faces)
        return

    asset_temp = args.assets_out + ".tmp"
    map_temp = out_path + ".tmp"
    for temp_path in (asset_temp, map_temp):
        if os.path.exists(temp_path):
            os.remove(temp_path)
    texture_ids, texture_meta, sector_visuals, palette = world_assets.emit_world_assets(
        asset_temp, map_data.texture_usage, map_data.sectors)
    report = bsp_emit.emit_map_c(map_temp, args.wad, map_data, texture_ids, texture_meta)
    os.replace(asset_temp, args.assets_out)
    os.replace(map_temp, out_path)

    print("Wrote %s" % out_path)
    print("  vertices : %d" % len(map_data.vertices))
    print("  segs     : %d flat solid/interactive (of %d source)" %
          (len(map_data.out_segs), map_data.source_seg_count))
    print("  linedefs : %d" % len(map_data.linedefs))
    print("  sectors  : %d" % len(map_data.sectors))
    print("  textures : %d exact + fallback" % (len(texture_ids) - 1))
    print("  palette  : %s" % " ".join("%02X%02X%02X" % color for color in palette))
    print("  assets   : %s" % args.assets_out)
    print("  subsectors: %d" % len(map_data.out_ssectors))
    print("  nodes    : %d (root=%d)" % (len(map_data.nodes), report.root))
    print("  blockmap : %dx%d, %d refs, %d validation queries" %
          (report.grid_width, report.grid_height, report.grid_ref_count,
           report.spatial_checks))
    print("  player   : (%d,%d) angle_deg=%d -> %d" % (
        map_data.start_x, map_data.start_y, map_data.start_angle_deg,
        map_data.start_angle))
    print("  things   : %d raw, %d curated, %d skipped" %
          (len(map_data.out_things), map_data.supported_things,
           len(map_data.out_things) - map_data.supported_things))
    print("  doors    : %d groups, faces=%s, fallback faces=%d" %
          (map_data.next_door_group, dict(sorted(map_data.door_face_counts.items())),
           map_data.fallback_door_faces))
    print("  keys     : available=0x%02X required=0x%02X reached=%s" %
          (map_data.certificate["available_keys"], map_data.required_key_mask,
           map_data.certificate["reached_masks"]))
    print("  certified: exit seg %d reachable with key mask=0x%02X after %d states" %
          (map_data.certificate["exit_index"], map_data.certificate["key_mask"],
           map_data.certificate["states"]))


if __name__ == "__main__":
    main()
