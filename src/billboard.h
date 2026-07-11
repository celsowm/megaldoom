#ifndef MEGALDOOM_BILLBOARD_H
#define MEGALDOOM_BILLBOARD_H

#include <genesis.h>
#include "raycast.h"

#define BILLBOARD_MAX_PROJECTED_OBJECTS 12

typedef enum {
    BILLBOARD_VISUAL_BONUS = 0,
    BILLBOARD_VISUAL_BLUE_KEY = 1,
    BILLBOARD_VISUAL_YELLOW_KEY = 2,
    BILLBOARD_VISUAL_RED_KEY = 3,
    BILLBOARD_VISUAL_STIMPACK = 4,
    BILLBOARD_VISUAL_MEDIKIT = 5,
    BILLBOARD_VISUAL_GREEN_ARMOR = 6,
    BILLBOARD_VISUAL_BLUE_ARMOR = 7,
    BILLBOARD_VISUAL_CLIP = 8,
    BILLBOARD_VISUAL_AMMO_BOX = 9,
    BILLBOARD_VISUAL_CANDLE = 10,
    BILLBOARD_VISUAL_CANDELABRA = 11,
    BILLBOARD_VISUAL_COLUMN = 12,
    BILLBOARD_VISUAL_BARREL = 13,
    BILLBOARD_VISUAL_TREE = 14,
    BILLBOARD_VISUAL_DUMMY = 15,
    BILLBOARD_VISUAL_DUMMY_DAMAGED = 16
} BillboardVisualId;

typedef struct {
    s16 left;
    s16 right;
    s16 top;
    s16 bottom;
    u16 depth;
    u8 visual_id;
    u8 frame;
} ProjectedBillboard;

typedef struct {
    u16 bonus;
    u16 key;
} BillboardPickupCounts;

typedef struct {
    u16 health;
    u16 armor;
    u16 ammo;
} BillboardPlayerStats;

typedef enum {
    BILLBOARD_PICKUP_NONE = 0,
    BILLBOARD_PICKUP_BONUS = 1,
    BILLBOARD_PICKUP_KEY = 2
} BillboardPickupKind;

typedef enum {
    BILLBOARD_EFFECT_NONE = 0,
    BILLBOARD_EFFECT_HEALTH = 1,
    BILLBOARD_EFFECT_ARMOR = 2,
    BILLBOARD_EFFECT_AMMO = 3,
    BILLBOARD_EFFECT_KEY = 4
} BillboardEffect;

typedef struct {
    bool collected;
    u8 effect;
    u16 amount;
    BillboardPickupKind kind;
} BillboardPickupResult;

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
BillboardPickupResult billboard_collect_near(s32 x, s32 y);
bool billboard_consume_key(void);
u16 billboard_get_collected_count(void);
BillboardPickupCounts billboard_get_pickup_counts(void);
BillboardPickupKind billboard_get_last_pickup_kind(void);
u16 billboard_get_enemy_count(void);
u16 billboard_get_active_count(void);
u16 billboard_get_target_count(void);
u16 billboard_get_target_health(void);
BillboardShotResult billboard_fire_center(const PlayerState *player, u16 wall_depth);
BillboardEnemyUpdate billboard_update_enemies(const PlayerState *player);
bool billboard_position_blocked(s32 x, s32 y, s32 radius);
#if DEBUG_PERF
u16 billboard_get_debug_culled_count(void);
u16 billboard_get_debug_projected_count(void);
u16 billboard_get_debug_prop_collision_candidates(void);
void billboard_debug_reset_stats(void);
#endif
u16 billboard_project_scene(const PlayerState *player,
                            ProjectedBillboard *objects,
                            u16 max_objects);

#endif
