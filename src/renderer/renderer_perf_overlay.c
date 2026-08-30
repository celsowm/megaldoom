#include "renderer_internal.h"
#include "renderer_perf.h"
#include "bsp_render.h"
#include "debug_checkpoint.h"

#if DEBUG_PERF

// The overlay never draws on BG_A and never draws over the 3D view.
//
//   * BG_A is whole-plane scrolled for weapon bob (renderer_frame_overlay.c
//     renderer_apply_weapon_bob: +-10px horizontally, 0..10px vertically), so
//     text committed there swings with the gun every step the player takes.
//     Neither plane used here scrolls: BG_B's only VDP_set*Scroll calls in the
//     tree are frontend.c's (both 0), and the window plane cannot scroll at all.
//   * The view is centred at rows VIEW_TILEMAP_Y..+VIEW_TILE_H-1 (5..19), so
//     the black letterbox is now SPLIT: VIEW_TILEMAP_Y rows above it (0..4) and
//     VIEW_GUTTER_TILE_H rows below it (20..23) before the status bar. The
//     overlay is split to match -- the top band on BG_B, the bottom band on the
//     window plane, which owns rows VIEW_WINDOW_TOP_Y.. and is what clips the
//     weapon dip. Text there rides above the (invisible) dipping gun and needs
//     no coordination with it.
//
// Both bands together must hold PERF_OVERLAY_H rows; the static check below and
// tools/test-active-battle-perf.py enforce it.
#define PERF_OVERLAY_W 40
#define PERF_OVERLAY_H 9
#define PERF_OVERLAY_TOP_H VIEW_TILEMAP_Y
#define PERF_OVERLAY_BOTTOM_H (PERF_OVERLAY_H - PERF_OVERLAY_TOP_H)
#define PERF_OVERLAY_REFRESH_FRAMES 30

#if PERF_OVERLAY_BOTTOM_H > VIEW_GUTTER_TILE_H
#error "Perf overlay does not fit in the letterbox bands around the 3D view"
#endif

static u16 s_perf_tilemap[PERF_OVERLAY_W * PERF_OVERLAY_H];
static char s_line[PERF_OVERLAY_W];
static u16 s_cursor;
static u16 s_host_fps;
static u16 s_host_cpu;

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

// SYS_getFPS counts the number of times it is called per second, so it has to
// be sampled once per game iteration -- reading it only on the overlay's
// 30-frame refresh would report ~1 FPS. Called from main.c's loop, which is
// where VDP_showFPS/VDP_showCPULoad used to draw straight onto (bobbing) BG_A.
void renderer_perf_overlay_sample_host(u32 frame) {
    s_host_fps = (u16)SYS_getFPS();
    // The CPU-load estimate is the more expensive of the two and moves slowly;
    // keep the every-8th-frame cadence VDP_showCPULoad had.
    if ((frame & 7) == 0) s_host_cpu = SYS_getCPULoad();
}

// Field legend, in screen order. Labels dropped their '=' when the overlay was
// repacked from 11 rows into the 9 letterbox rows that clear the viewport -- the
// ~40 '=' characters were worth a whole row, and no counter was dropped. Rows
// 0..4 land above the view, rows 5..8 in the gutter below it.
//
//   row 0  V   total vblanks this frame        A   average vblanks
//          P95 95th-percentile vblanks         X   missed deadlines
//          Mx  worst vblanks seen              FPS SYS_getFPS
//          CPU SYS_getCPULoad, percent
//   row 1  G   gameplay subticks               C   cast subticks
//          P   pack subticks                   R   projection subticks
//   row 2  B   billboard subticks              W   weapon subticks
//          Es  enemy-separation subticks
//          Ov  overlay restored/touched/overlapping tiles
//   row 3  D   dirty tiles                     U   tiles uploaded
//          R   upload runs                     M   view tiles modified
//          N|F|P - A|I   upload size (none/full/partial) - bank (active/inactive)
//          RR  redraw reason bits (hex)        Db  diagnostics subticks
//   row 4  Nv  BSP nodes visited               Br  boxes rejected cheaply
//          Bp  boxes projected                 Nf  near fallbacks
//          St  segments tested/drawn
//   row 5  Pc  player-collision subticks       Ec  enemy-collision subticks
//          Lo  line-of-sight subticks          K   collision/LOS candidates
//   row 6  O   active billboards               C   candidates
//          W   occluded                        D   projected
//          A   simulated enemies               H|M visibility cache hits/misses
//          Qc  projection cache hits/misses
//   row 7  Dp  deep phase label                Dt  its subticks
//          Dn  its units                       As  asm compare tile,
//                                                  then checked/mismatch/canary
//   row 8  Ep  enemy pair tests/close pairs/separation attempts/moves
//          Bc  prop-collision calls/scanned/candidates
//
// Headroom if a counter is ever added: rows 5..19 of BG_B, columns 0..9 and
// 30..39, are also black gutters either side of the view (300 more characters).
// Use one of those rather than compacting these lines further.
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

    line_begin(); TXT("V"); DEC(perf.total_vblanks,2); TXT(" A");
    DEC(perf.average_vblanks_x10 / 10,2); put_char('.'); DEC(perf.average_vblanks_x10 % 10,1);
    TXT(" P95"); DEC(perf.p95_vblanks,2); TXT(" X"); DEC(perf.missed_deadlines,4);
    TXT(" Mx"); DEC(perf.max_vblanks,2); TXT(" FPS"); DEC(s_host_fps,2);
    TXT(" CPU"); DEC(s_host_cpu,3); line_commit(0);

    line_begin(); TXT("G"); DEC(perf.gameplay_subticks,5); TXT(" C");
    DEC(perf.cast_subticks,5); TXT(" P"); DEC(perf.pack_subticks,5); TXT(" R");
    DEC(perf.projection_subticks,5); line_commit(1);

    line_begin(); TXT("B"); DEC(perf.billboard_subticks,5); TXT(" W");
    DEC(perf.weapon_subticks,5); TXT(" Es");
    DEC(billboard_get_debug_enemy_separation_subticks(),5); TXT(" Ov");
    DEC(perf.overlay_restored_tiles,3); put_char('/'); DEC(perf.overlay_touched_tiles,3);
    put_char('/'); DEC(perf.overlay_overlap_tiles,3); line_commit(2);

    line_begin(); TXT("D"); DEC(perf.upload_dirty_tiles,3); TXT(" U");
    DEC(perf.upload_tiles,3); TXT(" R"); DEC(perf.upload_runs,2); TXT(" M");
    DEC(renderer_get_frame_modified_count(),3); put_char(' '); put_char(size_c);
    put_char('-'); put_char(bank_c); TXT(" RR"); put_hex(perf.redraw_reasons,2);
    TXT(" Db"); DEC(perf.diagnostics_subticks,4); line_commit(3);

    line_begin(); TXT("Nv"); DEC(bsp_get_debug_nodes_visited(),3); TXT(" Br");
    DEC(bsp_get_debug_boxes_rejected_cheap(),3); TXT(" Bp");
    DEC(bsp_get_debug_boxes_projected(),3); TXT(" Nf");
    DEC(bsp_get_debug_near_fallbacks(),3); TXT(" St");
    DEC(bsp_get_debug_segments_tested(),3); put_char('/');
    DEC(bsp_get_debug_segments_drawn(),3); line_commit(4);

    line_begin(); TXT("Pc"); DEC(bsp_get_debug_player_collision_subticks(),4);
    TXT(" Ec"); DEC(bsp_get_debug_enemy_collision_subticks(),4); TXT(" Lo");
    DEC(bsp_get_debug_los_subticks(),4); TXT(" K"); DEC(bsp_get_debug_collision_candidates(),3);
    put_char('/'); DEC(bsp_get_debug_los_candidates(),3); line_commit(5);

    line_begin(); TXT("O"); DEC(billboard_get_active_count(),2); TXT(" C");
    DEC(billboard_get_debug_candidate_count(),2); TXT(" W"); DEC(billboard_get_debug_occluded_count(),2);
    TXT(" D"); DEC(billboard_get_debug_projected_count(),2); TXT(" A");
    DEC(billboard_get_debug_simulated_enemy_count(),2); TXT(" H");
    DEC(billboard_get_debug_visibility_cache_hits(),2); TXT(" M");
    DEC(billboard_get_debug_visibility_cache_misses(),2); TXT(" Qc");
    DEC(billboard_get_debug_projection_cache_hits(),3); put_char('/');
    DEC(billboard_get_debug_projection_cache_misses(),3); line_commit(6);

    line_begin(); TXT("Dp:"); TXT(deep_labels[displayed_deep_phase]); TXT(" Dt");
    DEC(perf.deep_subticks[displayed_deep_phase],5); TXT(" Dn");
    DEC(perf.deep_units[displayed_deep_phase],3); TXT(" As");
    DEC(perf.asm_compare_tile,3); put_char(' '); DEC(perf.asm_checked_tiles,4);
    put_char('/'); DEC(perf.asm_mismatches,2); put_char('/');
    DEC(perf.asm_canary_failures,2); line_commit(7);

    line_begin(); TXT("Ep"); DEC(billboard_get_debug_enemy_pair_tests(),3);
    put_char('/'); DEC(billboard_get_debug_enemy_close_pairs(),3); put_char('/');
    DEC(billboard_get_debug_enemy_separation_attempts(),3); put_char('/');
    DEC(billboard_get_debug_enemy_separation_moves(),3); TXT(" Bc");
    DEC(billboard_get_debug_prop_collision_calls(),3); put_char('/');
    DEC(billboard_get_debug_prop_collision_scanned(),3); put_char('/');
    DEC(billboard_get_debug_prop_collision_candidates(),3); line_commit(8);

    // Rows 0..PERF_OVERLAY_TOP_H-1 into the letterbox above the view; the rest
    // into the window plane's gutter rows below it. renderer_hud_window_setup
    // blanks that gutter on a menu return, which the next refresh repaints.
    VDP_setTileMapDataRect(BG_B, s_perf_tilemap, 0, 0,
                           PERF_OVERLAY_W, PERF_OVERLAY_TOP_H, PERF_OVERLAY_W, CPU);
    VDP_setTileMapDataRect(WINDOW, &s_perf_tilemap[PERF_OVERLAY_TOP_H * PERF_OVERLAY_W],
                           0, VIEW_WINDOW_TOP_Y,
                           PERF_OVERLAY_W, PERF_OVERLAY_BOTTOM_H, PERF_OVERLAY_W, CPU);
    renderer_perf_record_diagnostics(getSubTick() - diagnostics_start);
}

#endif
