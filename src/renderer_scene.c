#include "renderer_pack_internal.h"
#include "renderer_perf.h"
#include "player_controller.h"

void renderer_scene_init(void) {
    pack_stage_reset();
    compass_reset();
    frame_overlay_reset();
    upload_state_init();
    renderer_overlay_reset();
    clear_all_view_banks_dirty();
}

void renderer_invalidate_scene(void) {
    compass_reset();
    frame_overlay_reset();
    upload_state_invalidate();
    pack_stage_reset();
    renderer_overlay_reset();
    clear_all_view_banks_dirty();
    g_view_dirty_bank_mask = 0;
}

void renderer_render_scene(const RayColumn *columns,
                           const PlayerState *player,
                           const RaySceneColors *scene_colors,
                           bool base_dirty,
                           bool weapon_flash,
                           bool damage_flash,
                           bool low_health_warning) {
    const bool rebuild_base = (bool)(base_dirty ||
        renderer_overlay_requires_base_rebuild());
#if DEBUG_PERF
    u32 stage_start = getSubTick();
    renderer_reset_frame_modified();
    renderer_perf_reset_overlay_tiles();
#endif
    ProjectedBillboard objects[BILLBOARD_MAX_PROJECTED_TOTAL];
    const u16 object_count = billboard_project_scene(
        player, columns, objects, BILLBOARD_MAX_PROJECTED_TOTAL);
#if DEBUG_PERF
    renderer_perf_set_projection_subticks(getSubTick() - stage_start);
#endif

    if (rebuild_base) {
        upload_request_bank_swap();
        renderer_prepare_full_base_upload();
        renderer_overlay_base_rebuilt();
#if DEBUG_PERF
        stage_start = getSubTick();
#endif
        build_bsp_tilemap(columns, scene_colors, g_view_tiles);
        draw_door_overlays(columns, g_view_tiles);
#if DEBUG_PERF
        renderer_perf_set_pack_subticks(getSubTick() - stage_start);
        stage_start = getSubTick();
#endif
    } else {
        g_view_dirty_bank_mask = (u16)(1u << g_view_vram_bank);
        renderer_overlay_restore_previous();
#if DEBUG_PERF
        renderer_perf_set_pack_subticks(0);
#endif
    }

    renderer_overlay_begin();
#if DEBUG_PERF
    const u32 bb_start = getSubTick();
#endif
    draw_projected_billboards(columns, objects, object_count);
#if DEBUG_PERF
    renderer_perf_set_billboard_subticks(getSubTick() - bb_start);
    const u32 wpn_start = getSubTick();
#endif
    draw_weapon_overlay(weapon_flash);
    if (damage_flash) {
        draw_overlay_ops(MEGALDOOM_DAMAGE_OVERLAY_OPS, MEGALDOOM_OVERLAY_OP_COUNT[0]);
    } else if (low_health_warning) {
        draw_overlay_ops(MEGALDOOM_LOW_HEALTH_OVERLAY_OPS, MEGALDOOM_OVERLAY_OP_COUNT[1]);
    }
    build_compass_tilemap(player->angle);
    renderer_overlay_finish();
#if DEBUG_PERF
    renderer_perf_set_weapon_subticks(getSubTick() - wpn_start);
#endif
}
