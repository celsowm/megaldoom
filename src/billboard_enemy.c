#include "billboard_internal.h"
#include "bsp_map.h"

#define ENEMY_RADIUS 24

static u8 s_simulated_enemy_indices[BILLBOARD_OBJECT_COUNT];
static u8 s_simulated_enemy_visibility[BILLBOARD_OBJECT_COUNT];
static u16 s_simulated_enemy_count;
#define SEPARATION_WAS_VISIBLE 0x01u
#define SEPARATION_MOVED 0x02u
#define SEPARATION_MOVED_LEFT 0x01u
#define SEPARATION_MOVED_RIGHT 0x02u
#if DEBUG_PERF
static u16 s_debug_pair_tests;
static u16 s_debug_close_pairs;
static u16 s_debug_separation_attempts;
static u16 s_debug_separation_moves;
static u32 s_debug_separation_subticks;
#endif

typedef struct {
    bool position_changed;
    bool pose_changed;
} EnemyVisualChange;

static s16 get_step_toward(s32 delta) {
    if (delta > 0) {
        return DUMMY_MOVE_STEP;
    }
    if (delta < 0) {
        return -DUMMY_MOVE_STEP;
    }

    return 0;
}

static bool is_position_blocked(s32 x, s32 y) {
#if DEBUG_PERF
    bsp_debug_set_query_owner(BSP_QUERY_ENEMY);
#endif
    return bsp_circle_blocked(x, y, ENEMY_RADIUS) ||
           billboard_position_blocked(x, y, ENEMY_RADIUS);
}

static bool try_move_dummy(u16 index, BillboardObject *object, s16 step_x, s16 step_y) {
    bool moved = FALSE;

    if ((step_x != 0) && !is_position_blocked(object->x + step_x, object->y)) {
        object->x += step_x;
        moved = TRUE;
    }

    if ((step_y != 0) && !is_position_blocked(object->x, object->y + step_y)) {
        object->y += step_y;
        moved = TRUE;
    }

    if (moved) billboard_invalidate_object_visibility(index);

    return moved;
}

static bool dummies_need_separation(const BillboardObject *left,
                                    const BillboardObject *right) {
    s32 dx;
    s32 dy;

    if (!left->active || !right->active) {
        return FALSE;
    }
    if ((left->type_id != BILLBOARD_TYPE_DUMMY) || (right->type_id != BILLBOARD_TYPE_DUMMY)) {
        return FALSE;
    }
    if ((left->life_state != ENEMY_ALIVE) || (right->life_state != ENEMY_ALIVE)) {
        return FALSE;
    }

    dx = right->x - left->x;
    dy = right->y - left->y;
    if ((dx < -DUMMY_SEPARATION_RANGE) || (dx > DUMMY_SEPARATION_RANGE) ||
        (dy < -DUMMY_SEPARATION_RANGE) || (dy > DUMMY_SEPARATION_RANGE)) {
        return FALSE;
    }
    if ((dx == 0) && (dy == 0)) {
        return FALSE;
    }

    return (bool)(((dx * dx) + (dy * dy)) <= DUMMY_SEPARATION_RANGE_SQ);
}

static u8 separate_dummies(u16 left_index, BillboardObject *left,
                           u16 right_index, BillboardObject *right) {
    const s32 dx = right->x - left->x;
    const s32 dy = right->y - left->y;
    u8 moved = 0;
    s16 left_step_x = 0;
    s16 left_step_y = 0;
    s16 right_step_x = 0;
    s16 right_step_y = 0;

    if (!dummies_need_separation(left, right)) {
        return 0;
    }

    if ((dx < 0 ? -dx : dx) >= (dy < 0 ? -dy : dy)) {
        left_step_x = (dx >= 0) ? -DUMMY_MOVE_STEP : DUMMY_MOVE_STEP;
        right_step_x = (s16)-left_step_x;
    } else {
        left_step_y = (dy >= 0) ? -DUMMY_MOVE_STEP : DUMMY_MOVE_STEP;
        right_step_y = (s16)-left_step_y;
    }

    if (try_move_dummy(left_index, left, left_step_x, left_step_y)) {
        moved |= SEPARATION_MOVED_LEFT;
    }
    if (try_move_dummy(right_index, right, right_step_x, right_step_y)) {
        moved |= SEPARATION_MOVED_RIGHT;
    }

    return moved;
}

static bool update_dummy_alive(u16 index, BillboardObject *object, const PlayerState *player, u16 *hits) {
    const s32 player_dx = player->x - object->x;
    const s32 player_dy = player->y - object->y;
    const s32 abs_player_dx = (player_dx < 0) ? -player_dx : player_dx;
    const s32 abs_player_dy = (player_dy < 0) ? -player_dy : player_dy;
    // Almost every map enemy is dormant and far away. Reject the bounding
    // square before the four libgcc-backed 32-bit multiplies needed by d^2.
    if (!object->has_last_seen &&
        (abs_player_dx > DUMMY_WAKE_RANGE || abs_player_dy > DUMMY_WAKE_RANGE)) {
        object->saw_player = FALSE;
        object->spot_cooldown = 0;
        object->anim_frame = 0;
        object->anim_timer = 0;
        return FALSE;
    }
    const s32 player_dist_sq = (player_dx * player_dx) + (player_dy * player_dy);
    const bool perception_candidate =
        (player_dist_sq <= DUMMY_WAKE_RANGE_SQ) || object->has_last_seen;
    const bool visible = perception_candidate && billboard_has_line_of_sight(index, player);
    const bool engaged = visible || object->has_last_seen;
    bool moved = FALSE;

    // A dormant enemy is completely outside the simulation working set: no
    // patrol, animation, cooldown churn, collision or redraw until it perceives
    // the player. This is the normal idle state for map-spawned enemies.
    if (!engaged) {
        object->saw_player = FALSE;
        object->spot_cooldown = 0;
        object->anim_frame = 0;
        object->anim_timer = 0;
        return FALSE;
    }

    if (object->move_cooldown > 0) {
        object->move_cooldown--;
    }
    if (object->attack_cooldown > 0) {
        object->attack_cooldown--;
    }
    if (object->spot_cooldown > 0) {
        object->spot_cooldown--;
    }
    if (object->attack_anim > 0) {
        object->attack_anim--;
    }

    // Walk cadence: free-run the leg cycle while engaged (independent of the
    // discrete move_cooldown steps so it looks smooth, not once-per-hop). The
    // attack pose overrides the walk pose while attack_anim is active. Idle/asleep
    // enemies hold the rest pose.
    if (object->attack_anim == 0) {
        if (object->anim_timer == 0) {
            object->anim_frame = (u8)((object->anim_frame + 1) & (ENEMY_WALK_FRAME_COUNT - 1));
            object->anim_timer = ENEMY_WALK_HOLD;
        } else {
            object->anim_timer--;
        }
    }

    if (visible) {
        if (!object->saw_player) {
            object->saw_player = TRUE;
            object->spot_cooldown = DUMMY_SPOT_DELAY_FRAMES;
        }
        object->last_seen_x = player->x;
        object->last_seen_y = player->y;
        object->has_last_seen = TRUE;
    } else {
        object->saw_player = FALSE;
        object->spot_cooldown = 0;
    }

    if (visible && (object->spot_cooldown == 0) && (player_dist_sq <= DUMMY_ATTACK_RANGE_SQ) &&
        (object->attack_cooldown == 0)) {
        object->attack_cooldown = DUMMY_ATTACK_COOLDOWN;
        object->move_cooldown = DUMMY_ATTACK_RECOVERY_FRAMES;
        object->attack_anim = ENEMY_ATTACK_ANIM_FRAMES;
        (*hits)++;
        return FALSE;
    }

    if (object->move_cooldown != 0) {
        return FALSE;
    }

    {
        const s32 home_dx = object->home_x - object->x;
        const s32 home_dy = object->home_y - object->y;
        const s32 home_dist_sq = (home_dx * home_dx) + (home_dy * home_dy);
        const s32 seen_dx = object->last_seen_x - object->x;
        const s32 seen_dy = object->last_seen_y - object->y;
        const s32 seen_dist_sq = (seen_dx * seen_dx) + (seen_dy * seen_dy);
        s16 step_x;
        s16 step_y;

        if ((home_dist_sq > DUMMY_LEASH_RANGE_SQ) && (home_dist_sq > DUMMY_HOME_RANGE_SQ)) {
            step_x = get_step_toward(home_dx);
            step_y = get_step_toward(home_dy);
        } else if (visible && (player_dist_sq > DUMMY_STOP_RANGE_SQ) && (player_dist_sq <= DUMMY_CHASE_RANGE_SQ)) {
            step_x = get_step_toward(player_dx);
            step_y = get_step_toward(player_dy);
        } else if (object->has_last_seen && (seen_dist_sq > DUMMY_LAST_SEEN_RANGE_SQ)) {
            step_x = get_step_toward(seen_dx);
            step_y = get_step_toward(seen_dy);
        } else if (home_dist_sq > DUMMY_HOME_RANGE_SQ) {
            step_x = get_step_toward(home_dx);
            step_y = get_step_toward(home_dy);
        } else if (!visible) {
            object->has_last_seen = FALSE;
            object->anim_frame = 0;
            object->anim_timer = 0;
            return FALSE;
        } else {
            return FALSE;
        }

        if ((step_x < 0 ? -step_x : step_x) >= (step_y < 0 ? -step_y : step_y)) {
            moved = try_move_dummy(index, object, step_x, 0);
            if (!moved) {
                moved = try_move_dummy(index, object, 0, step_y);
            } else {
                moved = try_move_dummy(index, object, 0, step_y) || moved;
            }
        } else {
            moved = try_move_dummy(index, object, 0, step_y);
            if (!moved) {
                moved = try_move_dummy(index, object, step_x, 0);
            } else {
                moved = try_move_dummy(index, object, step_x, 0) || moved;
            }
        }
    }

    if (moved) {
        object->move_cooldown = DUMMY_MOVE_INTERVAL;
    }

    return moved;
}

static bool enemy_affects_view(u16 index, const BillboardObject *object,
                               const PlayerState *player, s16 cos_a, s16 sin_a) {
    BillboardMeasure measure;

    if (!billboard_measure_object(player, cos_a, sin_a, object, &measure)) return FALSE;
    if ((measure.right < 0) || (measure.left >= RAY_VIEW_COLS)) return FALSE;
    return billboard_has_line_of_sight(index, player);
}

// Step the death sequence one frame; holds on the final pose (corpse) once done.
static void advance_death(BillboardObject *object) {
    if (object->life_state == ENEMY_DEAD) {
        return;
    }
    if (object->death_timer > 0) {
        object->death_timer--;
        return;
    }
    if ((object->death_index + 1) < ENEMY_DEATH_FRAME_COUNT) {
        object->death_index++;
        object->death_timer = ENEMY_DEATH_HOLD;
    } else {
        object->life_state = ENEMY_DEAD;
    }
}

// Wrapper over the live AI / death animation. Keep position and pose changes
// distinct so the caller can attribute redraws without re-running simulation.
static EnemyVisualChange update_dummy(u16 index, BillboardObject *object,
                                      const PlayerState *player, u16 *hits) {
    const u8 prev_frame = billboard_get_object_frame(object);
    EnemyVisualChange change = {FALSE, FALSE};

    if (object->life_state != ENEMY_ALIVE) {
        advance_death(object);
    } else {
        change.position_changed = update_dummy_alive(index, object, player, hits);
    }

    if (billboard_get_object_frame(object) != prev_frame) {
        change.pose_changed = TRUE;
    }

    return change;
}

BillboardEnemyUpdate billboard_update_enemies(const PlayerState *player,
                                              bool redraw_pending) {
    BillboardEnemyUpdate update = {FALSE, FALSE, FALSE, 0, 0, 0};
    s32 best_hit_dist = 0x7FFFFFFF;
    s16 cos_a = 0;
    s16 sin_a = 0;

    if (!redraw_pending) {
        cos_a = fx_cos(player->angle);
        sin_a = fx_sin(player->angle);
    }

    billboard_visibility_begin(player);
    s_simulated_enemy_count = 0;
#if DEBUG_PERF
    s_debug_pair_tests = 0;
    s_debug_close_pairs = 0;
    s_debug_separation_attempts = 0;
    s_debug_separation_moves = 0;
    s_debug_separation_subticks = 0;
#endif

    const u8 *enemy_indices = billboard_registry_enemy_indices();
    const u16 enemy_count = billboard_registry_enemy_count();
    for (u16 slot = 0; slot < enemy_count; slot++) {
        const u16 i = enemy_indices[slot];
        BillboardObject *object = &g_billboards[i];

        if (!object->active || (object->type_id != BILLBOARD_TYPE_DUMMY)) {
            continue;
        }
        // Persistent corpses remain active for rendering, but no longer pay
        // simulation, projection-for-redraw, LOS or attacker-distance work.
        if (object->life_state == ENEMY_DEAD) {
            continue;
        }

        const u16 hits_before = update.hits;
        const bool was_visible = redraw_pending ? FALSE :
            enemy_affects_view(i, object, player, cos_a, sin_a);
        const EnemyVisualChange change = update_dummy(i, object, player, &update.hits);
        const bool changed = change.position_changed || change.pose_changed;
        const bool now_visible = (!redraw_pending && changed) ?
            enemy_affects_view(i, object, player, cos_a, sin_a) : was_visible;

        if (!redraw_pending && changed && (was_visible || now_visible)) {
            update.moved = TRUE;
            update.position_changed = update.position_changed || change.position_changed;
            update.pose_changed = update.pose_changed || change.pose_changed;
        }
        if ((object->life_state == ENEMY_ALIVE) && object->has_last_seen) {
            s_simulated_enemy_indices[s_simulated_enemy_count] = (u8)i;
            s_simulated_enemy_visibility[s_simulated_enemy_count] =
                (!redraw_pending && (changed ? now_visible : was_visible)) ?
                    SEPARATION_WAS_VISIBLE : 0;
            s_simulated_enemy_count++;
        }

        if (update.hits > hits_before) {
            const s32 dx = player->x - object->x;
            const s32 dy = player->y - object->y;
            const s32 dist_sq = (dx * dx) + (dy * dy);
            if (dist_sq >= best_hit_dist) {
                continue;
            }
            best_hit_dist = dist_sq;
            update.push_x = (dx > 0) ? 1 : ((dx < 0) ? -1 : 0);
            update.push_y = (dy > 0) ? 1 : ((dy < 0) ? -1 : 0);
        }
    }

    // Pair separation is local to the simulation working set, never all map enemies.
#if DEBUG_PERF
    const u32 separation_start = getSubTick();
#endif
    for (u16 a = 0; a < s_simulated_enemy_count; a++) {
        const u16 i = s_simulated_enemy_indices[a];
        for (u16 b = (u16)(a + 1); b < s_simulated_enemy_count; b++) {
            const u16 j = s_simulated_enemy_indices[b];
            BillboardObject *left = &g_billboards[i];
            BillboardObject *right = &g_billboards[j];
#if DEBUG_PERF
            s_debug_pair_tests++;
#endif

            // Most engaged enemies are nowhere near each other. Reject those
            // pairs before paying for two sprite projections and visibility checks.
            if (!dummies_need_separation(left, right)) {
                continue;
            }
#if DEBUG_PERF
            s_debug_close_pairs++;
            s_debug_separation_attempts++;
#endif

            const u8 moved = separate_dummies(i, left, j, right);
            if (moved != 0) {
#if DEBUG_PERF
                s_debug_separation_moves++;
#endif
                if ((moved & SEPARATION_MOVED_LEFT) != 0) {
                    s_simulated_enemy_visibility[a] |= SEPARATION_MOVED;
                }
                if ((moved & SEPARATION_MOVED_RIGHT) != 0) {
                    s_simulated_enemy_visibility[b] |= SEPARATION_MOVED;
                }
            }
        }
    }
    // Separation can move one enemy through several close pairs. Evaluate its
    // final visibility only once, after all positions settle, and compare it
    // with the visibility captured immediately before pair resolution.
    if (!redraw_pending) {
        for (u16 slot = 0; slot < s_simulated_enemy_count; slot++) {
            const u8 visibility = s_simulated_enemy_visibility[slot];
            if ((visibility & SEPARATION_MOVED) == 0) continue;

            const u16 i = s_simulated_enemy_indices[slot];
            const bool now_visible = enemy_affects_view(
                i, &g_billboards[i], player, cos_a, sin_a);
            if ((visibility & SEPARATION_WAS_VISIBLE) != 0 || now_visible) {
                update.moved = TRUE;
                update.position_changed = TRUE;
            }
        }
    }
#if DEBUG_PERF
    s_debug_separation_subticks = getSubTick() - separation_start;
#endif

    return update;
}

#if DEBUG_PERF
u16 billboard_get_debug_simulated_enemy_count(void) { return s_simulated_enemy_count; }
u16 billboard_get_debug_enemy_pair_tests(void) { return s_debug_pair_tests; }
u16 billboard_get_debug_enemy_close_pairs(void) { return s_debug_close_pairs; }
u16 billboard_get_debug_enemy_separation_attempts(void) { return s_debug_separation_attempts; }
u16 billboard_get_debug_enemy_separation_moves(void) { return s_debug_separation_moves; }
u32 billboard_get_debug_enemy_separation_subticks(void) { return s_debug_separation_subticks; }
#endif
