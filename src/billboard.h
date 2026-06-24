#ifndef MEGALDOOM_BILLBOARD_H
#define MEGALDOOM_BILLBOARD_H

#include <genesis.h>
#include "raycast.h"

#define BILLBOARD_MAX_SPANS 192

typedef enum {
    BILLBOARD_VISUAL_BONUS = 0,
    BILLBOARD_VISUAL_KEY = 1,
    BILLBOARD_VISUAL_DECOR = 2,
    BILLBOARD_VISUAL_DECOR_DAMAGED = 3,
    BILLBOARD_VISUAL_DUMMY = 4,
    BILLBOARD_VISUAL_DUMMY_DAMAGED = 5
} BillboardVisualId;

typedef struct {
    s16 column;
    s16 top;
    s16 bottom;
    u16 depth;
    u8 tex_x;
    u8 visual_id;
} BillboardSpan;

typedef struct {
    u16 bonus;
    u16 key;
} BillboardPickupCounts;

typedef enum {
    BILLBOARD_PICKUP_NONE = 0,
    BILLBOARD_PICKUP_BONUS = 1,
    BILLBOARD_PICKUP_KEY = 2
} BillboardPickupKind;

typedef enum {
    BILLBOARD_SHOT_NONE = 0,
    BILLBOARD_SHOT_DAMAGE = 1,
    BILLBOARD_SHOT_KILL = 2
} BillboardShotResult;

typedef struct {
    bool moved;
    u16 hits;
    s16 push_x;
    s16 push_y;
} BillboardEnemyUpdate;

void billboard_init(u16 phase_index);
bool billboard_collect_near(s32 x, s32 y);
bool billboard_consume_key(void);
u16 billboard_get_collected_count(void);
BillboardPickupCounts billboard_get_pickup_counts(void);
BillboardPickupKind billboard_get_last_pickup_kind(void);
u16 billboard_get_enemy_count(void);
u16 billboard_get_target_count(void);
u16 billboard_get_target_health(void);
BillboardShotResult billboard_fire_center(const PlayerState *player, u16 wall_depth);
BillboardEnemyUpdate billboard_update_enemies(const PlayerState *player);
u16 billboard_project_scene(const PlayerState *player, BillboardSpan *spans, u16 max_spans);

#endif
