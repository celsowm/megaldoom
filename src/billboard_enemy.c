#include "billboard_internal.h"
#include "world_map.h"

static bool has_line_of_sight(const BillboardObject *object, const PlayerState *player) {
    const s32 dx = player->x - object->x;
    const s32 dy = player->y - object->y;
    const s32 adx = (dx < 0) ? -dx : dx;
    const s32 ady = (dy < 0) ? -dy : dy;
    const s32 max_delta = (adx > ady) ? adx : ady;
    const s32 steps = (max_delta / (FX_ONE / 4)) + 1;

    for (s32 i = 1; i < steps; i++) {
        const s32 sample_x = object->x + ((dx * i) / steps);
        const s32 sample_y = object->y + ((dy * i) / steps);
        const s16 cell_x = (s16)(sample_x / WORLD_CELL_SIZE);
        const s16 cell_y = (s16)(sample_y / WORLD_CELL_SIZE);

        if (world_map_is_solid(cell_x, cell_y)) {
            return FALSE;
        }
    }

    return TRUE;
}

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
    const s16 cell_x = (s16)(x / WORLD_CELL_SIZE);
    const s16 cell_y = (s16)(y / WORLD_CELL_SIZE);

    return world_map_is_solid(cell_x, cell_y);
}

static bool try_move_dummy(BillboardObject *object, s16 step_x, s16 step_y) {
    bool moved = FALSE;

    if ((step_x != 0) && !is_position_blocked(object->x + step_x, object->y)) {
        object->x += step_x;
        moved = TRUE;
    }

    if ((step_y != 0) && !is_position_blocked(object->x, object->y + step_y)) {
        object->y += step_y;
        moved = TRUE;
    }

    return moved;
}

static bool patrol_dummy(BillboardObject *object) {
    const s32 patrol_target_x = object->home_x + ((object->patrol_axis_x ? object->patrol_dir : 0) * DUMMY_PATROL_RANGE);
    const s32 patrol_target_y = object->home_y + ((object->patrol_axis_x ? 0 : object->patrol_dir) * DUMMY_PATROL_RANGE);
    const s32 dx = patrol_target_x - object->x;
    const s32 dy = patrol_target_y - object->y;
    s16 step_x = get_step_toward(dx);
    s16 step_y = get_step_toward(dy);
    bool moved;

    if ((dx == 0) && (dy == 0)) {
        object->patrol_dir = (s8)-object->patrol_dir;
        step_x = (s16)(object->patrol_axis_x ? (object->patrol_dir * DUMMY_MOVE_STEP) : 0);
        step_y = (s16)(object->patrol_axis_x ? 0 : (object->patrol_dir * DUMMY_MOVE_STEP));
    }

    moved = try_move_dummy(object, step_x, step_y);
    if (!moved) {
        object->patrol_dir = (s8)-object->patrol_dir;
        moved = try_move_dummy(object,
                               (s16)(object->patrol_axis_x ? (object->patrol_dir * DUMMY_MOVE_STEP) : 0),
                               (s16)(object->patrol_axis_x ? 0 : (object->patrol_dir * DUMMY_MOVE_STEP)));
    }

    if (moved) {
        object->move_cooldown = DUMMY_PATROL_INTERVAL;
    }

    return moved;
}

static bool separate_dummies(BillboardObject *left, BillboardObject *right) {
    const s32 dx = right->x - left->x;
    const s32 dy = right->y - left->y;
    const s32 dist_sq = (dx * dx) + (dy * dy);
    bool moved = FALSE;
    s16 left_step_x = 0;
    s16 left_step_y = 0;
    s16 right_step_x = 0;
    s16 right_step_y = 0;

    if (!left->active || !right->active) {
        return FALSE;
    }
    if ((left->type_id != BILLBOARD_TYPE_DUMMY) || (right->type_id != BILLBOARD_TYPE_DUMMY)) {
        return FALSE;
    }
    if ((dist_sq == 0) || (dist_sq > DUMMY_SEPARATION_RANGE_SQ)) {
        return FALSE;
    }

    if ((dx < 0 ? -dx : dx) >= (dy < 0 ? -dy : dy)) {
        left_step_x = (dx >= 0) ? -DUMMY_MOVE_STEP : DUMMY_MOVE_STEP;
        right_step_x = (s16)-left_step_x;
    } else {
        left_step_y = (dy >= 0) ? -DUMMY_MOVE_STEP : DUMMY_MOVE_STEP;
        right_step_y = (s16)-left_step_y;
    }

    moved = try_move_dummy(left, left_step_x, left_step_y) || moved;
    moved = try_move_dummy(right, right_step_x, right_step_y) || moved;

    return moved;
}

static bool update_dummy(BillboardObject *object, const PlayerState *player, u16 *hits) {
    const s32 player_dx = player->x - object->x;
    const s32 player_dy = player->y - object->y;
    const s32 player_dist_sq = (player_dx * player_dx) + (player_dy * player_dy);
    const s32 home_dx = object->home_x - object->x;
    const s32 home_dy = object->home_y - object->y;
    const s32 home_dist_sq = (home_dx * home_dx) + (home_dy * home_dy);
    const s32 seen_dx = object->last_seen_x - object->x;
    const s32 seen_dy = object->last_seen_y - object->y;
    const s32 seen_dist_sq = (seen_dx * seen_dx) + (seen_dy * seen_dy);
    const bool awake = (player_dist_sq <= DUMMY_WAKE_RANGE_SQ) || object->has_last_seen;
    const bool visible = has_line_of_sight(object, player);
    bool moved = FALSE;

    if (object->move_cooldown > 0) {
        object->move_cooldown--;
    }
    if (object->attack_cooldown > 0) {
        object->attack_cooldown--;
    }
    if (object->spot_cooldown > 0) {
        object->spot_cooldown--;
    }

    if (awake && visible) {
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

    if (awake && visible && (object->spot_cooldown == 0) && (player_dist_sq <= DUMMY_ATTACK_RANGE_SQ) &&
        (object->attack_cooldown == 0)) {
        object->attack_cooldown = DUMMY_ATTACK_COOLDOWN;
        object->move_cooldown = DUMMY_ATTACK_RECOVERY_FRAMES;
        (*hits)++;
        return FALSE;
    }

    if (object->move_cooldown != 0) {
        return FALSE;
    }

    {
        s16 step_x;
        s16 step_y;

        if ((home_dist_sq > DUMMY_LEASH_RANGE_SQ) && (home_dist_sq > DUMMY_HOME_RANGE_SQ)) {
            object->has_last_seen = FALSE;
            step_x = get_step_toward(home_dx);
            step_y = get_step_toward(home_dy);
        } else if (awake && visible && (player_dist_sq > DUMMY_STOP_RANGE_SQ) && (player_dist_sq <= DUMMY_CHASE_RANGE_SQ)) {
            step_x = get_step_toward(player_dx);
            step_y = get_step_toward(player_dy);
        } else if (object->has_last_seen && (seen_dist_sq > DUMMY_LAST_SEEN_RANGE_SQ)) {
            step_x = get_step_toward(seen_dx);
            step_y = get_step_toward(seen_dy);
        } else if (home_dist_sq > DUMMY_HOME_RANGE_SQ) {
            object->has_last_seen = FALSE;
            step_x = get_step_toward(home_dx);
            step_y = get_step_toward(home_dy);
        } else if (!awake || !visible) {
            object->has_last_seen = FALSE;
            return patrol_dummy(object);
        } else {
            return FALSE;
        }

        if ((step_x < 0 ? -step_x : step_x) >= (step_y < 0 ? -step_y : step_y)) {
            moved = try_move_dummy(object, step_x, 0);
            if (!moved) {
                moved = try_move_dummy(object, 0, step_y);
            } else {
                moved = try_move_dummy(object, 0, step_y) || moved;
            }
        } else {
            moved = try_move_dummy(object, 0, step_y);
            if (!moved) {
                moved = try_move_dummy(object, step_x, 0);
            } else {
                moved = try_move_dummy(object, step_x, 0) || moved;
            }
        }
    }

    if (moved) {
        object->move_cooldown = DUMMY_MOVE_INTERVAL;
    }

    return moved;
}

BillboardEnemyUpdate billboard_update_enemies(const PlayerState *player) {
    BillboardEnemyUpdate update = {FALSE, 0, 0, 0};
    s32 best_hit_dist = 0x7FFFFFFF;

    for (u16 i = 0; i < BILLBOARD_OBJECT_COUNT; i++) {
        BillboardObject *object = &g_billboards[i];
        const s32 dx = player->x - object->x;
        const s32 dy = player->y - object->y;
        const s32 dist_sq = (dx * dx) + (dy * dy);
        const u16 hits_before = update.hits;

        if (!object->active || (object->type_id != BILLBOARD_TYPE_DUMMY)) {
            continue;
        }

        if (update_dummy(object, player, &update.hits)) {
            update.moved = TRUE;
        }

        if ((update.hits > hits_before) && (dist_sq < best_hit_dist)) {
            best_hit_dist = dist_sq;
            update.push_x = (dx > 0) ? 1 : ((dx < 0) ? -1 : 0);
            update.push_y = (dy > 0) ? 1 : ((dy < 0) ? -1 : 0);
        }
    }

    for (u16 i = 0; i < BILLBOARD_OBJECT_COUNT; i++) {
        for (u16 j = (u16)(i + 1); j < BILLBOARD_OBJECT_COUNT; j++) {
            if (separate_dummies(&g_billboards[i], &g_billboards[j])) {
                update.moved = TRUE;
            }
        }
    }

    return update;
}
