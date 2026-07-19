#include "renderer_internal.h"
#include "renderer_perf.h"
#include "bsp_render.h"
#include "debug_checkpoint.h"

#if DEBUG_PERF

#define PERF_OVERLAY_W 40
#define PERF_OVERLAY_H 11
#define PERF_OVERLAY_REFRESH_FRAMES 30

static u16 s_perf_tilemap[PERF_OVERLAY_W * PERF_OVERLAY_H];
static char s_line[PERF_OVERLAY_W];
static u16 s_cursor;

static void line_begin(void) {
    for (u16 i = 0; i < PERF_OVERLAY_W; i++) s_line[i] = ' ';
    s_cursor = 0;
}

static void put_char(char value) {
    if (s_cursor < PERF_OVERLAY_W) s_line[s_cursor++] = value;
}

static void put_text(const char *value) {
    while (*value != 0 && s_cursor < PERF_OVERLAY_W) put_char(*value++);
}

static void put_dec(u32 value, u16 width) {
    static const u16 powers[5] = {10000, 1000, 100, 10, 1};
    const u16 start = (width >= 5) ? 0 : (u16)(5 - width);
    // divu traps when the quotient does not fit in 16 bits. Stage counters are
    // displayed as saturated 16-bit values, which is also enough to expose a
    // missed frame without making the diagnostics path unsafe.
    if (value > 65535u) value = 65535u;
    for (u16 i = start; i < 5; i++) {
        const u16 digit = divu(value, powers[i]);
        put_char((char)('0' + digit));
        value -= (u32)digit * powers[i];
    }
}

static void put_hex(u16 value, u16 width) {
    static const char digits[] = "0123456789ABCDEF";
    while (width > 0) {
        width--;
        put_char(digits[(value >> (width * 4)) & 0x0F]);
    }
}

static void line_commit(u16 row) {
    u16 *target = &s_perf_tilemap[row * PERF_OVERLAY_W];
    for (u16 i = 0; i < PERF_OVERLAY_W; i++) {
        target[i] = TILE_ATTR_FULL(PAL0, FALSE, FALSE, FALSE,
            TILE_FONT_INDEX + ((u8)s_line[i] - 32));
    }
}

#define TXT(v) put_text(v)
#define DEC(v,w) put_dec((u32)(v), (w))

void renderer_draw_perf_overlay(bool frame_complete) {
    static const char *const deep_labels[RENDERER_PERF_DEEP_COUNT] = {
        "BS", "BX", "BR", "PM", "PF", "BP"
    };
    static u16 refresh_counter;
    RendererPerfSnapshot perf;
    u32 diagnostics_start;
    char size_c;
    char bank_c;
    u16 displayed_deep_phase;

    if (!frame_complete) return;
    refresh_counter++;
    if ((refresh_counter != 1) && (refresh_counter < PERF_OVERLAY_REFRESH_FRAMES)) return;
    refresh_counter = 0;
    diagnostics_start = getSubTick();
    perf = renderer_get_perf_snapshot();
    debug_checkpoint_publish_perf(&perf, sizeof(perf));
    size_c = (perf.upload_dirty_tiles == 0) ? 'N' : (perf.upload_full ? 'F' : 'P');
    bank_c = perf.upload_swap ? 'I' : 'A';
    displayed_deep_phase = (perf.deep_phase == 0) ?
        (RENDERER_PERF_DEEP_COUNT - 1) : (u16)(perf.deep_phase - 1);

    line_begin(); TXT("V="); DEC(perf.total_vblanks,2); TXT(" Av=");
    DEC(perf.average_vblanks_x10 / 10,2); put_char('.'); DEC(perf.average_vblanks_x10 % 10,1);
    TXT(" P95="); DEC(perf.p95_vblanks,2); TXT(" Vm="); DEC(perf.max_vblanks,2);
    TXT(" X="); DEC(perf.missed_deadlines,4); line_commit(0);

    line_begin(); TXT("G="); DEC(perf.gameplay_subticks,5); TXT(" C=");
    DEC(perf.cast_subticks,5); TXT(" P="); DEC(perf.pack_subticks,5); line_commit(1);

    line_begin(); TXT("Pr="); DEC(perf.projection_subticks,5); TXT(" B=");
    DEC(perf.billboard_subticks,5); TXT(" W="); DEC(perf.weapon_subticks,5); line_commit(2);

    line_begin(); TXT("D="); DEC(perf.upload_dirty_tiles,3); TXT(" U=");
    DEC(perf.upload_tiles,3); TXT(" R="); DEC(perf.upload_runs,2); TXT(" M=");
    DEC(renderer_get_frame_modified_count(),3); put_char(' '); put_char(size_c); put_char('-'); put_char(bank_c);
    line_commit(3);

    line_begin(); TXT("N="); DEC(bsp_get_debug_nodes_visited(),3); TXT(" R=");
    DEC(bsp_get_debug_boxes_rejected_cheap(),3); TXT(" P=");
    DEC(bsp_get_debug_boxes_projected(),3); TXT(" F="); DEC(bsp_get_debug_near_fallbacks(),3);
    TXT(" S="); DEC(bsp_get_debug_segments_tested(),3); put_char('/');
    DEC(bsp_get_debug_segments_drawn(),3); line_commit(4);

    line_begin(); TXT("PC="); DEC(bsp_get_debug_player_collision_subticks(),4);
    TXT(" EC="); DEC(bsp_get_debug_enemy_collision_subticks(),4); TXT(" L=");
    DEC(bsp_get_debug_los_subticks(),4); TXT(" K="); DEC(bsp_get_debug_collision_candidates(),3);
    put_char('/'); DEC(bsp_get_debug_los_candidates(),3); line_commit(5);

    line_begin(); TXT("O"); DEC(billboard_get_active_count(),2); TXT(" C");
    DEC(billboard_get_debug_candidate_count(),2); TXT(" W"); DEC(billboard_get_debug_occluded_count(),2);
    TXT(" D"); DEC(billboard_get_debug_projected_count(),2); TXT(" A");
    DEC(billboard_get_debug_simulated_enemy_count(),2); TXT(" H");
    DEC(billboard_get_debug_visibility_cache_hits(),2); TXT(" M");
    DEC(billboard_get_debug_visibility_cache_misses(),2); line_commit(6);

    line_begin(); TXT("Dp="); TXT(deep_labels[displayed_deep_phase]); TXT(" T=");
    DEC(perf.deep_subticks[displayed_deep_phase],5); TXT(" N=");
    DEC(perf.deep_units[displayed_deep_phase],3); TXT(" As=");
    DEC(perf.asm_compare_tile,3); put_char(' '); DEC(perf.asm_checked_tiles,4);
    put_char('/'); DEC(perf.asm_mismatches,2); put_char('/');
    DEC(perf.asm_canary_failures,2); line_commit(7);

    line_begin(); TXT("RR="); put_hex(perf.redraw_reasons,2); TXT(" Ov=");
    DEC(perf.overlay_restored_tiles,3); put_char('/'); DEC(perf.overlay_touched_tiles,3);
    put_char('/'); DEC(perf.overlay_overlap_tiles,3); TXT(" Db=");
    DEC(perf.diagnostics_subticks,4); line_commit(8);

    line_begin(); TXT("Qc="); DEC(billboard_get_debug_projection_cache_hits(),3);
    put_char('/'); DEC(billboard_get_debug_projection_cache_misses(),3); TXT(" Ep=");
    DEC(billboard_get_debug_enemy_pair_tests(),3); put_char('/');
    DEC(billboard_get_debug_enemy_close_pairs(),3); put_char('/');
    DEC(billboard_get_debug_enemy_separation_attempts(),3); put_char('/');
    DEC(billboard_get_debug_enemy_separation_moves(),3); line_commit(9);

    line_begin(); TXT("Es="); DEC(billboard_get_debug_enemy_separation_subticks(),5);
    TXT(" Bc="); DEC(billboard_get_debug_prop_collision_calls(),3); put_char('/');
    DEC(billboard_get_debug_prop_collision_scanned(),3); put_char('/');
    DEC(billboard_get_debug_prop_collision_candidates(),3); line_commit(10);

    VDP_setTileMapDataRect(BG_A, s_perf_tilemap, 0, 1,
                           PERF_OVERLAY_W, PERF_OVERLAY_H, PERF_OVERLAY_W, CPU);
    renderer_perf_record_diagnostics(getSubTick() - diagnostics_start);
}

#endif
