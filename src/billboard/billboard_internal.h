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
// Conservative horizontal radius for the visible-subsector prototype. The
// largest authored billboard extent is barrel explosion BEXPE (31 world units
// right of its origin) at visual_scale 2 = 62; 96 covers every source patch.
#define BILLBOARD_SUBSECTOR_CULL_RADIUS 96
// Point-blank barrel detonation fallback. billboard_measure_object rejects
// anything within BILLBOARD_MIN_DEPTH of the camera forward axis, but a barrel
// you are pressed against must still be shootable in Doom fashion: the Euclidean
// fallback in billboard_fire_center detonates the nearest live barrel inside
// this radius when the aim scan picks nothing. Player+barrel collision radii
// sum to 36u; ~2.5x that (90u) keeps the trigger tolerant without detonating
// barrels across the room when the player shoots blank walls.
#define BILLBOARD_POINT_BLANK_RADIUS 90
#define BILLBOARD_POINT_BLANK_RADIUS_SQ (BILLBOARD_POINT_BLANK_RADIUS * BILLBOARD_POINT_BLANK_RADIUS)
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
// Enemy atlas pixels use native Doom world units, enlarged 2.25x on-screen
// (2026-07-27: 24x48 native read too small in play, then a same-day 3x pass
// [72x144] read too tall -- settled 25% down from that, at 2.25x). The atlas
// art itself stays 24x48 (ROM texel source); only the projected world size
// grows, so the same pixels are stretched over a bigger screen box, same as
// the pickup 3x path below. The 2.25x is now carried per pose by
// ENEMY_FRAME_GEOMETRY (see below) instead of one 54x108 box, so resizing the
// enemy means rescaling that table, not editing a pair of constants.
#define BILLBOARD_ENEMY_ATLAS_WIDTH 24
#define BILLBOARD_ENEMY_ATLAS_HEIGHT 48
// WAD patch geometry and BSP vertices share native Doom map units. Individual
// types may enlarge only their rendered patch through BillboardType.visual_scale;
// collision, collection radii and world coordinates remain native.
#define BILLBOARD_WORLD_GEOMETRY_SCALE 1
#define BILLBOARD_PICKUP_VISUAL_SCALE 3
// 2026-08-07: dropped from 3x to shrink barrel sprites (~33% smaller, the
// closest this integer-only multiplier gets to a 30% reduction). Collision
// radius (BILLBOARD_TYPES' `radius` field, 20u) and explosion AoE
// (BARREL_EXPLOSION_RADIUS below) are separate fields and are deliberately
// NOT scaled by this constant.
#define BILLBOARD_BARREL_VISUAL_SCALE 2

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

// Barrel explosion sequence (5 BEXP frames A0..E0). Indexing mirrors the
// enemy death frames: billboard_get_object_frame returns 0..4 for dying
// barrels and billboard_get_object_visual_id flips to
// BILLBOARD_VISUAL_BARREL_EXPLODING so renderer_scene.c draws from
// FREEDOOM_BILLBOARD_BARREL_EXPLOSION_FRAMES instead of the static BAR1 slot.
#define BARREL_DEATH_FRAME_COUNT 5
static const u8 BARREL_DEATH_FRAME_HOLDS[BARREL_DEATH_FRAME_COUNT] = {1, 1, 1, 1, 1};

// Barrel explosion AoE. World sprites are visually enlarged, but using their
// full 3x scale here made barrels kill the player from across a room. A 192-unit
// reach keeps point-blank splash reliable without the excessive 384-unit range.
#define BARREL_EXPLOSION_BASE_RADIUS 128
#define BARREL_EXPLOSION_RADIUS 192
#define BARREL_EXPLOSION_DAMAGE 128
// Per-explosion worklist cap. E1M1's largest barrel cluster is 3; this still
// bounds worst-case chains without unbounded recursion.
#define BARREL_EXPLOSION_MAX_CHAIN 8

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

// Per-pose billboard geometry, in world units, indexed by the pose numbers
// above. Same {source_w, source_h, left_offset, top_offset} shape as
// FREEDOOM_BILLBOARD_BARREL_EXPLOSION_GEOMETRY, and read through the same
// single WAD-origin projection path every billboard uses: top_offset is how far
// the pose reaches ABOVE the floor line, source_h - top_offset how far below
// (negative => the whole pose is airborne, which is what a mid-collapse death
// frame is in Doom).
//
// Before 2026-08-07 every pose was stretched over one fixed 54x108 box, so the
// 17px-tall POSSL0 corpse was smeared over a standing body's height and its
// visible mass floated at chest level instead of resting on the floor. The
// stretched-to-fill 24x48 ROM atlas cells are correct as-is; it was the
// DESTINATION rect that was wrong, so no asset regeneration is involved.
//
// Derived from res/originaldoom/sprites/_offsets.json by
// tools/test-billboard-enemy-geometry.py, which re-derives and asserts every
// row. Scale factors are pinned to the walk pose so this correction left the
// standing enemy exactly the size it was tuned to (see the 2.25x note above):
// Sx = 54/POSSA1.width = 54/41, Sy = 108/POSSA1.height = 108/55, and the
// baseline is lifted by POSSA1's 5 native units of below-origin overhang.
#define ENEMY_FRAME_GEOMETRY_COUNT 10
static const s16 ENEMY_FRAME_GEOMETRY[ENEMY_FRAME_GEOMETRY_COUNT][4] = {
    { 54, 108, 27, 108},  // 0 POSSA1 walk   41x55 ty50
    { 49, 108, 25, 112},  // 1 POSSB1 walk   37x55 ty52
    { 50, 108, 25, 108},  // 2 POSSC1 walk   38x55 ty50
    { 53, 108, 27, 112},  // 3 POSSD1 walk   40x55 ty52
    { 36, 108, 18, 108},  // 4 POSSF1 attack 27x55 ty50
    { 51, 108, 26, 112},  // 5 POSSH0 death  39x55 ty52
    { 46,  90, 23, 102},  // 6 POSSI0 death  35x46 ty47
    { 57,  66, 29,  92},  // 7 POSSJ0 death  43x34 ty42  (airborne mid-collapse)
    { 63,  53, 32,  67},  // 8 POSSK0 death  48x27 ty29  (airborne mid-collapse)
    { 62,  33, 31,  33},  // 9 POSSL0 corpse 47x17 ty12  (top_offset == source_h
};                        //                              => flat on the floor)

// Animation cadence (frames at the locked 30fps): ~4 tics/pose like Doom.
#define ENEMY_WALK_HOLD 4
#define ENEMY_DEATH_HOLD 5
#define ENEMY_ATTACK_ANIM_FRAMES 10

typedef struct {
    u8 visual_id;
    u8 effect;
    u8 hit_points;
    u8 radius;
    u8 visual_scale;
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
const u8 *billboard_registry_target_indices(void);
const u8 *billboard_registry_blocking_indices(void);
u16 billboard_registry_active_count(void);
u16 billboard_registry_enemy_count(void);
u16 billboard_registry_target_count(void);
u16 billboard_registry_blocking_count(void);
u16 billboard_registry_living_enemy_count(void);

#endif
