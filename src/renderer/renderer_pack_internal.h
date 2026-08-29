#ifndef MEGALDOOM_RENDERER_PACK_INTERNAL_H
#define MEGALDOOM_RENDERER_PACK_INTERNAL_H

// Private cross-file contract for the renderer_scene.c split. Only the
// renderer_{flats,pack,doors,billboard_draw,frame_overlay,upload,
// sparse,scene}.c translation units include this header - it is not part of
// the renderer's public API (renderer.h / renderer_internal.h).
#include "renderer_internal.h"
#include "bsp_render.h"
#include "renderer_pack_abi.h"

// Per-column wall/door description, produced once per sampled column by the
// pack stage (renderer_pack.c) and consumed by the pack stage itself, door
// overlay compositing (renderer_doors.c), and the sparse classifier
// (renderer_sparse.c).
typedef struct {
    u16 top;
    u16 bottom;
    const u8 *texture;
    const u8 *shade_map;
    const u8 *vertical_samples;
    u8 tex_x;
    u8 tex_y;
    u8 texture_id;
    u8 shade_level;
    u8 flags;
    u8 texture_height;
    u16 v_scale_q12;
} WallColumnDescriptor;

// renderer_hotpath.s indexes this struct by the offsets in renderer_pack_abi.h.
// The asm cannot use offsetof, so these are the checks that keep the two views
// of the layout from drifting apart silently.
_Static_assert(__builtin_offsetof(WallColumnDescriptor, top) == WALL_DESC_OFF_TOP,
               "asm wall post reads top at WALL_DESC_OFF_TOP");
_Static_assert(__builtin_offsetof(WallColumnDescriptor, bottom) == WALL_DESC_OFF_BOTTOM,
               "asm wall post reads bottom at WALL_DESC_OFF_BOTTOM");
_Static_assert(__builtin_offsetof(WallColumnDescriptor, vertical_samples) ==
                   WALL_DESC_OFF_VERTICAL_SAMPLES,
               "asm wall post reads the DDA at WALL_DESC_OFF_VERTICAL_SAMPLES");
_Static_assert(__builtin_offsetof(WallColumnDescriptor, tex_y) == WALL_DESC_OFF_TEX_Y,
               "asm wall post reads tex_y at WALL_DESC_OFF_TEX_Y");
_Static_assert(sizeof(WallColumnDescriptor) == WALL_DESC_SIZE,
               "asm advances one lane by WALL_DESC_SIZE");

static inline u16 wall_source_y(const WallColumnDescriptor *descriptor,
                                u16 rel_y) {
    u16 tex_y = (u16)(((u32)descriptor->vertical_samples[rel_y] *
                       descriptor->v_scale_q12) >> 12);
    const u16 offset = (u16)(((u32)descriptor->tex_y *
                              descriptor->v_scale_q12) >> 12);
    tex_y = (u16)(tex_y + offset);
    if (descriptor->texture_height == WALL_TEX_HEIGHT) return tex_y;
    if (tex_y >= descriptor->texture_height) {
        tex_y = (u16)(tex_y - descriptor->texture_height);
    }
    return tex_y;
}

static inline u16 wall_packed_y(const WallColumnDescriptor *descriptor,
                                u16 rel_y) {
    return (u16)((descriptor->vertical_samples[rel_y] + descriptor->tex_y) &
                 WALL_TEX_HEIGHT_MASK);
}

WallColumnDescriptor describe_wall_column(const RayColumn *column);
WallColumnDescriptor describe_door_overlay(const RayDoorOverlay *door);

// Pre-packed ceiling/floor rows for the current frame's scene colors
// (renderer_flats.c), consumed by the pack stage.
// Field order mirrors PACK_FLAT_OFF_* -- floor first; see the addressing note
// in renderer_pack_abi.h before reordering these.
typedef struct {
    u32 floor[4];
    const u32 *ceiling; // PACK_CEILING_ROW_COUNT rows, in ROM
} PackedFlatRows;

// Same contract as above: the asm's two flat posts index this by byte.
_Static_assert(__builtin_offsetof(PackedFlatRows, ceiling) == PACK_FLAT_OFF_CEILING,
               "asm flat posts read ceiling at PACK_FLAT_OFF_CEILING");
_Static_assert(__builtin_offsetof(PackedFlatRows, floor) == PACK_FLAT_OFF_FLOOR,
               "asm flat posts read floor at PACK_FLAT_OFF_FLOOR");
_Static_assert(MEGALDOOM_SKY_CEILING_ROW_COUNT == PACK_CEILING_ROW_COUNT,
               "both ROM ceiling tables must match the asm's index mask");
_Static_assert(sizeof(((PackedFlatRows *)0)->floor) == PACK_FLOOR_ROWS_BYTES,
               "asm wraps the floor index with PACK_FLOOR_INDEX_MASK");
// The ceiling table must cover every screen row a ceiling run can reach.
// describe_wall_column clamps a column to 1 <= height <= VIEW_PIXEL_H and the
// packer centres it (top = (VIEW_PIXEL_H - height) / 2), so the highest wall
// top -- and therefore the last ceiling row ever written -- is at
// (VIEW_PIXEL_H - 1) / 2. write_ceiling_tile reads eight consecutive rows
// UNMASKED from that index, so the table must also hold the remainder of that
// tile; both bounds are asserted here rather than trusted.
_Static_assert(PACK_CEILING_ROW_COUNT > ((VIEW_PIXEL_H - 1) / 2),
               "ceiling table must reach the highest centred wall top");
_Static_assert((PACK_CEILING_ROW_COUNT & (PACK_CEILING_ROW_COUNT - 1)) == 0,
               "asm masks the ceiling index with an immediate power of two");

// build_flat_rows is a pure function of the scene's two RayFlatColor triples
// and its sky bit, so comparing those seven bytes proves the packed tables are
// unchanged. Compare the INPUTS rather than the output: the ceiling table is
// PACK_CEILING_ROW_COUNT entries, and a full-table compare every frame would
// cost more than the invalidation it guards.
static inline bool scene_flats_equal(const RaySceneColors *a,
                                     const RaySceneColors *b) {
    return (bool)(a->ceiling.primary == b->ceiling.primary &&
                  a->ceiling.secondary == b->ceiling.secondary &&
                  a->ceiling.secondary_coverage == b->ceiling.secondary_coverage &&
                  a->floor.primary == b->floor.primary &&
                  a->floor.secondary == b->floor.secondary &&
                  a->floor.secondary_coverage == b->floor.secondary_coverage &&
                  a->sky == b->sky);
}

// Returns a pointer into a cached static; valid until the next call.
const PackedFlatRows *build_flat_rows(const RaySceneColors *scene_colors);
void flat_rows_invalidate(void);

// Pixel-replication table for the active stride (guarded so the unused one
// isn't compiled): REP4[c] == c*0x1111 spreads a colour across 4px (stride 4);
// REP2[c] == c*0x11 spreads it across 2px (stride 2, four cast columns per
// 8px tile). Defined in renderer_flats.c.
#if RAY_COL_STRIDE == 4
extern const u32 REP4[16];
#else
extern const u32 REP2[16];
#endif

// Tile bands always begin at y%4 == 0, so the four prepacked Bayer rows repeat
// exactly twice. Three MOVEM.L instructions replace two eight-iteration C
// loops for every whole ceiling/floor tile without changing a nibble.
static inline void write_repeated_flat_tile(u32 *target, const u32 rows[4]) {
    __asm__ volatile (
        "movem.l (%1),%%d0-%%d3\n\t"
        "movem.l %%d0-%%d3,(%0)\n\t"
        "movem.l %%d0-%%d3,16(%0)"
        :
        : "a" (target), "a" (rows)
        : "d0", "d1", "d2", "d3", "memory");
}

// Ceiling variant: the 128-row table is indexed by absolute screen row, so a
// whole-ceiling tile copies its own eight rows rather than repeating four.
// One extra MOVEM.L load over write_repeated_flat_tile; still no per-row loop.
static inline void write_ceiling_tile(u32 *target, const u32 *ceiling,
                                      u16 pixel_y) {
    const u32 *rows = &ceiling[pixel_y];
    __asm__ volatile (
        "movem.l (%1),%%d0-%%d3\n\t"
        "movem.l %%d0-%%d3,(%0)\n\t"
        "movem.l 16(%1),%%d0-%%d3\n\t"
        "movem.l %%d0-%%d3,16(%0)"
        :
        : "a" (target), "a" (rows)
        : "d0", "d1", "d2", "d3", "memory");
}

// The BSP tile packer itself (renderer_pack.c).
void build_bsp_tilemap(const RayColumn *columns,
                       const RaySceneColors *scene_colors,
                       u32 target[][8]);

// Resets the pack stage's shade LUT and coherence cache. Called from
// renderer_scene_init()/renderer_invalidate_scene().
void pack_stage_reset(void);

// Door overlay compositing (renderer_doors.c).
void draw_door_overlays(const RayColumn *columns, u32 target[][8]);
bool door_overlay_blocks_pixel(const RayColumn *column, u16 object_depth, u16 y);

// Billboard rasterization into g_view_tiles (renderer_billboard_draw.c).
void draw_projected_billboards(const RayColumn *columns,
                               const ProjectedBillboard *objects,
                               u16 object_count);

// Weapon sprite + damage/low-health screen tint (renderer_frame_overlay.c).
void draw_weapon_overlay(bool flash);
// Re-upload the CURRENTLY selected weapon's tileset into the shared VRAM window
// without changing the selection. Used by renderer_init and by the post-pause
// restore, which both have to repopulate a window the menus may have clobbered.
void reload_weapon_tiles(void);
void draw_overlay_ops(const MegalDoomOverlayRowOp *ops, u16 count);
void frame_overlay_reset(void);

// VRAM DMA upload scheduling (renderer_upload.c).
void clear_all_view_banks_dirty(void);
void load_view_tile_run(u16 vram_base, u16 first, u16 count);
void upload_state_init(void);
void upload_state_invalidate(void);
void upload_request_bank_swap(void);

// Sparse Semantic Framebuffer classify/build (renderer_sparse.c). Dead code
// while RENDERER_SPARSE_FB == 0; declared unconditionally so renderer_upload.c
// can call them without its own #if RENDERER_SPARSE_FB guard duplication.
void sparse_classify_frame(const RayColumn *columns,
                           const RaySceneColors *scene_colors,
                           SparseFrameBuild *build);
void sparse_build_tilemap(const RayColumn *columns,
                          const RaySceneColors *scene_colors,
                          u16 sector, u16 bank,
                          u16 *screen_tilemap,
                          const SparseFrameBuild *build);
#if RENDERER_SPARSE_FB
void sparse_queue_dynamic_runs(const SparseFrameBuild *build, u16 inactive_bank_base);
#endif

#endif
