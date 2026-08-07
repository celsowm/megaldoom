#ifndef MEGALDOOM_RENDERER_PACK_INTERNAL_H
#define MEGALDOOM_RENDERER_PACK_INTERNAL_H

// Private cross-file contract for the renderer_scene.c split. Only the
// renderer_{flats,pack,doors,billboard_draw,frame_overlay,upload,
// sparse,scene}.c translation units include this header - it is not part of
// the renderer's public API (renderer.h / renderer_internal.h).
#include "renderer_internal.h"
#include "bsp_render.h"

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
} WallColumnDescriptor;

WallColumnDescriptor describe_wall_column(const RayColumn *column);
WallColumnDescriptor describe_door_overlay(const RayDoorOverlay *door);

// Pre-packed ceiling/floor rows for the current frame's scene colors
// (renderer_flats.c), consumed by the pack stage.
typedef struct {
    u32 ceiling[4];
    u32 floor[4];
} PackedFlatRows;

PackedFlatRows build_flat_rows(const RaySceneColors *scene_colors);

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
