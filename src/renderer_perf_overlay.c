#include "renderer_internal.h"
#include "renderer_perf.h"
#include "bsp_render.h"

#if DEBUG_PERF

#define PERF_OVERLAY_REFRESH_FRAMES 4

void renderer_draw_perf_overlay(bool frame_complete) {
    static u16 refresh_counter;
    char text[44];
    RendererPerfSnapshot perf;

    if (!frame_complete) return;
    refresh_counter++;
    if ((refresh_counter != 1) &&
        ((refresh_counter & (PERF_OVERLAY_REFRESH_FRAMES - 1)) != 0)) return;

    perf = renderer_get_perf_snapshot();
    const char size_c = (perf.upload_dirty_tiles == 0) ? 'N'
                        : (perf.upload_full ? 'F' : 'P');
    const char bank_c = perf.upload_swap ? 'I' : 'A';

    sprintf(text, "V=%u Vm=%u X=%u Vup=0 %c-%c",
            (unsigned int)perf.total_vblanks,
            (unsigned int)perf.max_vblanks,
            (unsigned int)perf.missed_deadlines,
            size_c, bank_c);
    VDP_drawTextFill(text, 0, 1, 32);

    sprintf(text, "G=%04lu C=%04lu P=%04lu",
            (unsigned long)perf.gameplay_subticks,
            (unsigned long)perf.cast_subticks,
            (unsigned long)perf.pack_subticks);
    VDP_drawTextFill(text, 0, 2, 32);

    sprintf(text, "Pr=%04lu B=%04lu W=%04lu",
            (unsigned long)perf.projection_subticks,
            (unsigned long)perf.billboard_subticks,
            (unsigned long)perf.weapon_subticks);
    VDP_drawTextFill(text, 0, 3, 32);

    sprintf(text, "D=%03u U=%03u R=%02u M=%03u Up=%04lu Ud=%04lu",
            (unsigned int)perf.upload_dirty_tiles,
            (unsigned int)perf.upload_tiles,
            (unsigned int)perf.upload_runs,
            (unsigned int)renderer_get_frame_modified_count(),
            (unsigned long)perf.upload_prepare_subticks,
            (unsigned long)perf.dma_wait_subticks);
    VDP_drawTextFill(text, 0, 4, 40);

    sprintf(text, "N=%03u R=%03u P=%03u F=%03u S=%03u/%03u",
            (unsigned int)bsp_get_debug_nodes_visited(),
            (unsigned int)bsp_get_debug_boxes_rejected_cheap(),
            (unsigned int)bsp_get_debug_boxes_projected(),
            (unsigned int)bsp_get_debug_near_fallbacks(),
            (unsigned int)bsp_get_debug_segments_tested(),
            (unsigned int)bsp_get_debug_segments_drawn());
    VDP_drawTextFill(text, 0, 5, 40);

    sprintf(text, "PC=%03lu EC=%03lu L=%03lu K=%03u LK=%03u",
            (unsigned long)bsp_get_debug_player_collision_subticks(),
            (unsigned long)bsp_get_debug_enemy_collision_subticks(),
            (unsigned long)bsp_get_debug_los_subticks(),
            (unsigned int)bsp_get_debug_collision_candidates(),
            (unsigned int)bsp_get_debug_los_candidates());
    VDP_drawTextFill(text, 0, 6, 40);

    sprintf(text, "O%02u C%02u W%02u D%02u A%02u H%02u M%02u",
            (unsigned int)billboard_get_active_count(),
            (unsigned int)billboard_get_debug_candidate_count(),
            (unsigned int)billboard_get_debug_occluded_count(),
            (unsigned int)billboard_get_debug_projected_count(),
            (unsigned int)billboard_get_debug_simulated_enemy_count(),
            (unsigned int)billboard_get_debug_visibility_cache_hits(),
            (unsigned int)billboard_get_debug_visibility_cache_misses());
    VDP_drawTextFill(text, 0, 7, 40);

    sprintf(text, "Bx=%04lu Sr=%04lu Sc=%04lu",
            (unsigned long)bsp_get_debug_box_projection_subticks(),
            (unsigned long)bsp_get_debug_segment_raster_subticks(),
            (unsigned long)bsp_get_debug_side_cache_subticks());
    VDP_drawTextFill(text, 0, 8, 40);

    sprintf(text, "RR=%02X Ov=%03u/%03u/%03u",
            (unsigned int)perf.redraw_reasons,
            (unsigned int)perf.overlay_restored_tiles,
            (unsigned int)perf.overlay_touched_tiles,
            (unsigned int)perf.overlay_overlap_tiles);
    VDP_drawTextFill(text, 0, 9, 40);

    sprintf(text, "Qc=%03u/%03u Ep=%03u/%03u/%03u/%03u",
            (unsigned int)billboard_get_debug_projection_cache_hits(),
            (unsigned int)billboard_get_debug_projection_cache_misses(),
            (unsigned int)billboard_get_debug_enemy_pair_tests(),
            (unsigned int)billboard_get_debug_enemy_close_pairs(),
            (unsigned int)billboard_get_debug_enemy_separation_attempts(),
            (unsigned int)billboard_get_debug_enemy_separation_moves());
    VDP_drawTextFill(text, 0, 10, 40);

    sprintf(text, "Es=%04lu Bc=%03u/%03u/%03u",
            (unsigned long)billboard_get_debug_enemy_separation_subticks(),
            (unsigned int)billboard_get_debug_prop_collision_calls(),
            (unsigned int)billboard_get_debug_prop_collision_scanned(),
            (unsigned int)billboard_get_debug_prop_collision_candidates());
    VDP_drawTextFill(text, 0, 11, 40);
}

#endif
