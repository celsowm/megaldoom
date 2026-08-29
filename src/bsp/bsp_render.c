#include "bsp_render_internal.h"

// draw_seg only computes the height/texture fields at columns on a RAY_COL_STRIDE
// boundary, matching the columns build_bsp_tilemap actually samples. That
// relies on the stride being a power of two (the (x & (STRIDE-1)) test).
#if (RAY_COL_STRIDE & (RAY_COL_STRIDE - 1)) != 0
#error "draw_seg's strided-column optimization requires RAY_COL_STRIDE to be a power of two"
#endif

s16 g_fwx, g_fwy;
s16 g_rx, g_ry;
s32 g_px, g_py;
u8 g_next_open[BSP_SAMPLE_COLS + 1];
u32 g_solid_words[BSP_SOLID_WORD_COUNT];
u16 g_solid_count;
s16 g_vertex_depth[BSP_MAX_VERTICES];
s16 g_vertex_lateral[BSP_MAX_VERTICES];
u8 g_vertex_generation[BSP_MAX_VERTICES];
u8 g_cast_generation;
RayColumn *g_columns;
u8 g_node_side_bits[(BSP_MAX_NODES + 7) / 8];
u8 g_node_side_generation[BSP_MAX_NODES];
u8 g_position_generation;
s32 g_node_cache_px, g_node_cache_py;
bool g_node_cache_valid;

#if DEBUG_PERF
u16 g_bsp_dbg_nodes_visited, g_bsp_dbg_boxes_rejected_cheap;
u16 g_bsp_dbg_boxes_projected, g_bsp_dbg_near_fallbacks;
u16 g_bsp_dbg_segments_tested, g_bsp_dbg_segments_drawn;
u16 g_bsp_dbg_visible_subsectors;
u32 g_bsp_dbg_side_cache_subticks;
u32 g_bsp_dbg_box_projection_subticks;
u32 g_bsp_dbg_segment_raster_subticks;
bool g_bsp_dbg_measure_side, g_bsp_dbg_measure_box;
bool g_bsp_dbg_measure_segment;
#endif

#if DEBUG_PERF || BILLBOARD_VISIBLE_SUBSECTOR_CULL
u8 g_visible_subsector_bits[(BSP_MAX_SUBSECTORS + 7) / 8];
#endif

void bsp_invalidate_node_cache(void) {
    g_node_cache_valid = FALSE;
}

void bsp_init(void) {
    // The near/far order cache is rebuilt on the first bsp_cast_frame (position
    // will differ from the bss-zeroed sentinel). Collision/LOS still computes
    // seg AABBs inline from the vertex table (no per-seg RAM on the 64KB MD).
    g_node_cache_valid = FALSE;
    g_position_generation = 1;
    for (u16 i = 0; i < BSP_MAX_NODES; i++) g_node_side_generation[i] = 0;
    g_cast_generation = 0;
    for (u16 i = 0; i < BSP_MAX_VERTICES; i++) g_vertex_generation[i] = 0;
}

void bsp_cast_frame(const PlayerState *player, RayColumn *columns, RaySceneColors *scene_colors) {
    g_columns = columns;

#if DEBUG_PERF || BILLBOARD_VISIBLE_SUBSECTOR_CULL
    for (u16 i = 0; i < (BSP_MAX_SUBSECTORS + 7) / 8; i++) {
        g_visible_subsector_bits[i] = 0;
    }
#endif

    const u16 subsector = bsp_find_subsector(player->x, player->y);
    u16 sector = (subsector < bsp_subsector_count) ? bsp_subsector_sector[subsector] : 0;
    if (sector >= FREEDOOM_SECTOR_VISUAL_COUNT) sector = 0;
    const u8 *visual = FREEDOOM_SECTOR_VISUALS[sector];
    scene_colors->ceiling = (RayFlatColor){visual[0], visual[1], visual[2]};
    scene_colors->floor = (RayFlatColor){visual[3], visual[4], visual[5]};
    scene_colors->sector = sector;

    // Clear occlusion and seed every column with a far/empty default so columns
    // no wall covers still render (as distant, mostly sky/floor).
    g_solid_count = 0;
    for (u16 i = 0; i < BSP_SOLID_WORD_COUNT; i++) {
        g_solid_words[i] = 0;
    }
    // Only door.height needs clearing up front: it is the sentinel every door
    // consumer short-circuits on (draw_seg here, draw_door_overlays and
    // column_door_active in the pack stage), and draw_seg writes the whole
    // RayDoorOverlay whenever it writes height, so the other six door fields are
    // never read while height is 0. The wall fields are deferred to
    // bsp_seed_unclaimed_columns below.
    for (u16 sample = 0; sample < BSP_SAMPLE_COLS; sample++) {
        g_next_open[sample] = (u8)sample;
        columns[(u16)(sample * RAY_COL_STRIDE)].door.height = 0;
    }
    g_next_open[BSP_SAMPLE_COLS] = BSP_SAMPLE_COLS;

    bsp_traverse_front_to_back(player);
    bsp_seed_unclaimed_columns(columns);
}

#if DEBUG_PERF || BILLBOARD_VISIBLE_SUBSECTOR_CULL
bool bsp_subsector_was_visited(u16 subsector_id) {
    if (subsector_id >= BSP_MAX_SUBSECTORS) return FALSE;
    return (bool)(g_visible_subsector_bits[subsector_id >> 3] &
                  (u8)(1u << (subsector_id & 7)));
}
#endif

#if DEBUG_PERF
// These accessors are consumed only by the optional perf overlay in main.c.
// Keep them visible across SGDK's LTO link even though this translation unit
// itself has no callers for them.
#define BSP_DEBUG_EXPORT __attribute__((used, externally_visible))
BSP_DEBUG_EXPORT u16 bsp_get_debug_nodes_visited(void) { return g_bsp_dbg_nodes_visited; }
BSP_DEBUG_EXPORT u16 bsp_get_debug_boxes_rejected_cheap(void) { return g_bsp_dbg_boxes_rejected_cheap; }
BSP_DEBUG_EXPORT u16 bsp_get_debug_boxes_projected(void) { return g_bsp_dbg_boxes_projected; }
BSP_DEBUG_EXPORT u16 bsp_get_debug_near_fallbacks(void) { return g_bsp_dbg_near_fallbacks; }
BSP_DEBUG_EXPORT u16 bsp_get_debug_segments_tested(void) { return g_bsp_dbg_segments_tested; }
BSP_DEBUG_EXPORT u16 bsp_get_debug_segments_drawn(void) { return g_bsp_dbg_segments_drawn; }
BSP_DEBUG_EXPORT u16 bsp_get_debug_visible_subsector_count(void) {
    return g_bsp_dbg_visible_subsectors;
}
BSP_DEBUG_EXPORT u32 bsp_get_debug_side_cache_subticks(void) { return g_bsp_dbg_side_cache_subticks; }
BSP_DEBUG_EXPORT u32 bsp_get_debug_box_projection_subticks(void) { return g_bsp_dbg_box_projection_subticks; }
BSP_DEBUG_EXPORT u32 bsp_get_debug_segment_raster_subticks(void) { return g_bsp_dbg_segment_raster_subticks; }
#undef BSP_DEBUG_EXPORT
#endif
