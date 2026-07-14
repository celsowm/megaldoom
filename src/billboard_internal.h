#ifndef MEGALDOOM_BILLBOARD_INTERNAL_H
#define MEGALDOOM_BILLBOARD_INTERNAL_H

#include "billboard.h"
#include "bsp_map.h"
#include "fixed_math.h"

#define BILLBOARD_MIN_DEPTH 32
#define BILLBOARD_MAX_DEPTH (6 * FX_ONE)
#define BILLBOARD_PHASE_COUNT 2
// View dimensions come from raycast.h (RAY_VIEW_COLS/ROWS) -- billboard.h
// includes it -- instead of being redefined here.
#define BILLBOARD_OBJECT_COUNT 112
#define BILLBOARD_COLLECT_RADIUS (FX_ONE / 2)
#define BILLBOARD_COLLECT_RADIUS_SQ (BILLBOARD_COLLECT_RADIUS * BILLBOARD_COLLECT_RADIUS)
#define BILLBOARD_TYPE_BONUS 0
#define BILLBOARD_TYPE_KEY 1
#define BILLBOARD_TYPE_STIMPACK 2
#define BILLBOARD_TYPE_MEDIKIT 3
#define BILLBOARD_TYPE_ARMOR_BONUS 4
#define BILLBOARD_TYPE_GREEN_ARMOR 5
#define BILLBOARD_TYPE_BLUE_ARMOR 6
#define BILLBOARD_TYPE_CLIP 7
#define BILLBOARD_TYPE_AMMO_BOX 8
#define BILLBOARD_TYPE_CANDLE 9
#define BILLBOARD_TYPE_CANDELABRA 10
#define BILLBOARD_TYPE_COLUMN 11
#define BILLBOARD_TYPE_ELEC 12
#define BILLBOARD_TYPE_BARREL 13
#define BILLBOARD_TYPE_TREE 14
#define BILLBOARD_TYPE_DUMMY 15
#define BILLBOARD_TYPE_COUNT 16
#define BILLBOARD_PROP_RADIUS 16
#define BILLBOARD_ENEMY_WORLD_WIDTH 34
#define BILLBOARD_ENEMY_WORLD_HEIGHT 102
#define BILLBOARD_ENEMY_ATLAS_WIDTH 24
#define BILLBOARD_ENEMY_ATLAS_HEIGHT 48

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
#define DUMMY_SPOT_DELAY_FRAMES 12
#define DUMMY_SEPARATION_RANGE (FX_ONE / 2)
#define DUMMY_SEPARATION_RANGE_SQ (DUMMY_SEPARATION_RANGE * DUMMY_SEPARATION_RANGE)
#define DUMMY_LAST_SEEN_RANGE (FX_ONE / 8)
#define DUMMY_LAST_SEEN_RANGE_SQ (DUMMY_LAST_SEEN_RANGE * DUMMY_LAST_SEEN_RANGE)

// Enemy life state (BillboardObject.life_state).
#define ENEMY_ALIVE 0
#define ENEMY_DYING 1
#define ENEMY_DEAD 2

// Enemy sprite pose indices into FREEDOOM_BILLBOARD_ENEMY_FRAMES: 0..3 walk,
// 4 attack, 5..9 death (last = persistent corpse).
#define ENEMY_WALK_FRAME_COUNT 4
#define ENEMY_ATTACK_FRAME_INDEX 4
#define ENEMY_DEATH_FRAME_BASE 5
#define ENEMY_DEATH_FRAME_COUNT 5

// Animation cadence (frames at the locked 30fps): ~4 tics/pose like Doom.
#define ENEMY_WALK_HOLD 4
#define ENEMY_DEATH_HOLD 5
#define ENEMY_ATTACK_ANIM_FRAMES 10

typedef struct {
    u8 visual_id;
    u8 effect;
    u8 hit_points;
    u8 radius;
    s32 max_depth;
    bool collectible;
    bool targetable;
    bool blocking;
} BillboardType;

typedef struct {
    s16 source_w;
    s16 source_h;
    s16 left_offset;
    s16 top_offset;
    u8 atlas_x;
    u8 atlas_y;
    u8 atlas_w;
    u8 atlas_h;
    bool uses_wad_origin;
} BillboardGeometry;

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
    bool saw_player;
    u8 spot_cooldown;
    s32 last_seen_x;
    s32 last_seen_y;
    bool has_last_seen;
    u8 life_state;
    u8 anim_frame;
    u8 anim_timer;
    u8 death_index;
    u8 death_timer;
    u8 attack_anim;
} BillboardObject;

typedef struct {
    const BillboardType *type;
    s32 forward;
    s32 side;
    s16 center_col;
    s16 half_w;
    s16 projected_height;
    s16 left;
    s16 right;
    s16 top;
    s16 bottom;
    u8 atlas_x;
    u8 atlas_y;
    u8 atlas_w;
    u8 atlas_h;
} BillboardMeasure;

extern BillboardObject g_billboards[];
extern u16 g_collected_count;
extern BillboardPickupCounts g_pickup_counts;
extern BillboardPickupKind g_last_pickup_kind;

const BillboardType *billboard_get_type(u8 type_id);
u8 billboard_get_object_visual_id(const BillboardObject *object, const BillboardType *type);
u8 billboard_get_object_frame(const BillboardObject *object);
bool billboard_measure_object(const PlayerState *player, s16 cos_a, s16 sin_a,
                              const BillboardObject *object, BillboardMeasure *measure);
void billboard_visibility_begin(const PlayerState *player);
bool billboard_has_line_of_sight(u16 index, const PlayerState *player);
void billboard_invalidate_object_visibility(u16 index);
void billboard_registry_reset(void);
void billboard_registry_add(u16 index);
void billboard_registry_deactivate(u16 index);
void billboard_registry_enemy_died(u16 index);
const u8 *billboard_registry_active_indices(void);
const u8 *billboard_registry_enemy_indices(void);
u16 billboard_registry_active_count(void);
u16 billboard_registry_enemy_count(void);
u16 billboard_registry_living_enemy_count(void);

#endif
