#ifndef BSP_RENDER_INTERNAL_H
#define BSP_RENDER_INTERNAL_H

#include "bsp_render.h"
#include "bsp_map.h"
#include "fixed_math.h"
#include "generated_assets.h"
#include "renderer_perf.h"
#include "bsp_inv_depth_lut.h"
#include "debug_checkpoint.h"

#define LEFT_REJECT_SCALE (RAY_VIEW_CENTER_X + RAY_COL_STRIDE + 1)
#define RIGHT_REJECT_SCALE (RAY_VIEW_COLS + RAY_COL_STRIDE - RAY_VIEW_CENTER_X)
#define BSP_NEAR 16
#define BSP_INV_SCALE 16384
#define BSP_SAMPLE_COLS (RAY_VIEW_COLS / RAY_COL_STRIDE)
#define BSP_SOLID_WORD_COUNT ((BSP_SAMPLE_COLS + 31) / 32)

extern s16 g_fwx, g_fwy;
extern s16 g_rx, g_ry;
extern s32 g_px, g_py;
extern u8 g_next_open[BSP_SAMPLE_COLS + 1];
extern u32 g_solid_words[BSP_SOLID_WORD_COUNT];
extern u16 g_solid_count;
extern s16 g_vertex_depth[BSP_MAX_VERTICES];
extern s16 g_vertex_lateral[BSP_MAX_VERTICES];
extern u8 g_vertex_generation[BSP_MAX_VERTICES];
extern u8 g_cast_generation;
extern RayColumn *g_columns;
extern u8 g_node_side_bits[(BSP_MAX_NODES + 7) / 8];
extern u8 g_node_side_generation[BSP_MAX_NODES];
extern u8 g_position_generation;
extern s32 g_node_cache_px, g_node_cache_py;
extern bool g_node_cache_valid;

#if DEBUG_PERF
extern u16 g_bsp_dbg_nodes_visited, g_bsp_dbg_boxes_rejected_cheap;
extern u16 g_bsp_dbg_boxes_projected, g_bsp_dbg_near_fallbacks;
extern u16 g_bsp_dbg_segments_tested, g_bsp_dbg_segments_drawn;
extern u16 g_bsp_dbg_visible_subsectors;
extern u32 g_bsp_dbg_side_cache_subticks;
extern u32 g_bsp_dbg_box_projection_subticks;
extern u32 g_bsp_dbg_segment_raster_subticks;
extern bool g_bsp_dbg_measure_side, g_bsp_dbg_measure_box;
extern bool g_bsp_dbg_measure_segment;
#define BSP_DBG_INC(c) (g_bsp_dbg_##c)++
#define BSP_DBG_RESET() do { g_bsp_dbg_nodes_visited = 0; \
        g_bsp_dbg_boxes_rejected_cheap = 0; \
        g_bsp_dbg_boxes_projected = 0; \
        g_bsp_dbg_near_fallbacks = 0; \
        g_bsp_dbg_segments_tested = 0; \
        g_bsp_dbg_segments_drawn = 0; \
        g_bsp_dbg_visible_subsectors = 0; \
        g_bsp_dbg_side_cache_subticks = 0; \
        g_bsp_dbg_box_projection_subticks = 0; \
        g_bsp_dbg_segment_raster_subticks = 0; } while (0)
#elif CADENCE_STAGE_PROBE
#define BSP_DBG_INC(c) bsp_cadence_inc_##c()
static inline void bsp_cadence_inc_nodes_visited(void) { g_cadence_nodes_visited++; }
static inline void bsp_cadence_inc_boxes_projected(void) { g_cadence_boxes_projected++; }
static inline void bsp_cadence_inc_segments_tested(void) { g_cadence_segs_tested++; }
static inline void bsp_cadence_inc_segments_drawn(void) { g_cadence_segs_drawn++; }
static inline void bsp_cadence_inc_boxes_rejected_cheap(void) { g_cadence_box_cheap_reject++; }
static inline void bsp_cadence_inc_near_fallbacks(void) { g_cadence_box_near_path++; }
#define BSP_DBG_RESET() ((void)0)
#else
#define BSP_DBG_INC(c) ((void)0)
#define BSP_DBG_RESET() ((void)0)
#endif

#if DEBUG_PERF || BILLBOARD_VISIBLE_SUBSECTOR_CULL
extern u8 g_visible_subsector_bits[(BSP_MAX_SUBSECTORS + 7) / 8];
#endif

s32 bsp_native_muls_word(s16 left, s16 right);
s32 bsp_render_mul(s32 left, s32 right);
u32 bsp_native_mulu_word(u16 left, u16 right);
s32 bsp_native_mul_long_unsigned(s32 left, u16 right);
u16 bsp_reciprocal_depth(s32 depth);
s32 bsp_reciprocal_span(s32 span);
s32 bsp_perspective_divide(s32 numerator, s32 denominator);
void bsp_transform_vertex(u16 vertex_index, s32 *depth, s32 *lateral);

bool bsp_view_fully_closed(void);
u16 bsp_find_next_open(u16 sample);
void bsp_mark_sample_solid(u16 sample);
bool bsp_solid_sample_range_filled(u16 left_sample, u16 right_sample);
void bsp_seed_column_default(RayColumn *col);
void bsp_draw_seg(u16 seg_index);
void bsp_seed_unclaimed_columns(RayColumn *columns);

bool bsp_project_box_range(const BspBox *box, s16 *left, s16 *right);
void bsp_render_boxed_child(u16 child, const BspBox *box);
void bsp_render_node(u16 child);
void bsp_traverse_front_to_back(const PlayerState *player);
void bsp_visit_leaf(u16 subsector_id);

#endif
