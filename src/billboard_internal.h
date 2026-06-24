#ifndef MEGALDOOM_BILLBOARD_INTERNAL_H
#define MEGALDOOM_BILLBOARD_INTERNAL_H

#include "billboard.h"
#include "fixed_math.h"

#define BILLBOARD_MIN_DEPTH 32
#define BILLBOARD_MAX_DEPTH (6 * FX_ONE)
#define BILLBOARD_SCALE (3 * FX_ONE)
#define BILLBOARD_PHASE_COUNT 2
#define BILLBOARD_VIEW_COLS 32
#define BILLBOARD_VIEW_TILE_H 12
#define BILLBOARD_OBJECT_COUNT 7
#define BILLBOARD_COLLECT_RADIUS (FX_ONE / 2)
#define BILLBOARD_COLLECT_RADIUS_SQ (BILLBOARD_COLLECT_RADIUS * BILLBOARD_COLLECT_RADIUS)
#define BILLBOARD_TYPE_BONUS 0
#define BILLBOARD_TYPE_KEY 1
#define BILLBOARD_TYPE_DECOR 2
#define BILLBOARD_TYPE_DUMMY 3
#define DUMMY_MOVE_STEP (FX_ONE / 8)
#define DUMMY_MOVE_INTERVAL 5
#define DUMMY_STOP_RANGE (FX_ONE / 2)
#define DUMMY_STOP_RANGE_SQ (DUMMY_STOP_RANGE * DUMMY_STOP_RANGE)
#define DUMMY_ATTACK_RANGE ((FX_ONE * 3) / 4)
#define DUMMY_ATTACK_RANGE_SQ (DUMMY_ATTACK_RANGE * DUMMY_ATTACK_RANGE)
#define DUMMY_ATTACK_COOLDOWN 30
#define DUMMY_ATTACK_RECOVERY_FRAMES 10
#define DUMMY_WAKE_RANGE (4 * FX_ONE)
#define DUMMY_WAKE_RANGE_SQ (DUMMY_WAKE_RANGE * DUMMY_WAKE_RANGE)
#define DUMMY_CHASE_RANGE (6 * FX_ONE)
#define DUMMY_CHASE_RANGE_SQ (DUMMY_CHASE_RANGE * DUMMY_CHASE_RANGE)
#define DUMMY_HIT_PUSH_STEP (FX_ONE / 4)
#define DUMMY_HIT_STUN_FRAMES 10
#define DUMMY_HOME_RANGE (FX_ONE / 8)
#define DUMMY_HOME_RANGE_SQ (DUMMY_HOME_RANGE * DUMMY_HOME_RANGE)
#define DUMMY_LEASH_RANGE (2 * FX_ONE)
#define DUMMY_LEASH_RANGE_SQ (DUMMY_LEASH_RANGE * DUMMY_LEASH_RANGE)
#define DUMMY_PATROL_RANGE (FX_ONE / 2)
#define DUMMY_PATROL_INTERVAL 8
#define DUMMY_SPOT_DELAY_FRAMES 12
#define DUMMY_SEPARATION_RANGE (FX_ONE / 2)
#define DUMMY_SEPARATION_RANGE_SQ (DUMMY_SEPARATION_RANGE * DUMMY_SEPARATION_RANGE)
#define DUMMY_LAST_SEEN_RANGE (FX_ONE / 8)
#define DUMMY_LAST_SEEN_RANGE_SQ (DUMMY_LAST_SEEN_RANGE * DUMMY_LAST_SEEN_RANGE)

typedef struct {
    u8 visual_id;
    u8 damaged_visual_id;
    u8 hit_points;
    s32 scale;
    s32 max_depth;
    bool collectible;
    bool targetable;
} BillboardType;

typedef struct {
    s32 x;
    s32 y;
    u8 type_id;
    u8 hp;
    bool active;
    u8 move_cooldown;
    u8 attack_cooldown;
    s32 home_x;
    s32 home_y;
    s8 patrol_dir;
    bool patrol_axis_x;
    bool saw_player;
    u8 spot_cooldown;
    s32 last_seen_x;
    s32 last_seen_y;
    bool has_last_seen;
} BillboardObject;

typedef struct {
    const BillboardType *type;
    s32 forward;
    s32 side;
    s16 center_col;
    s16 half_w;
} BillboardMeasure;

extern const BillboardObject INITIAL_BILLBOARDS[BILLBOARD_PHASE_COUNT][BILLBOARD_OBJECT_COUNT];
extern BillboardObject g_billboards[];
extern u16 g_collected_count;
extern BillboardPickupCounts g_pickup_counts;
extern BillboardPickupKind g_last_pickup_kind;

const BillboardType *billboard_get_type(u8 type_id);
u8 billboard_get_object_visual_id(const BillboardObject *object, const BillboardType *type);
bool billboard_measure_object(const PlayerState *player, const BillboardObject *object, BillboardMeasure *measure);

#endif
