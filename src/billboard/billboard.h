#ifndef MEGALDOOM_BILLBOARD_H
#define MEGALDOOM_BILLBOARD_H

#include <genesis.h>
#include "doom_skill.h"
#include "raycast.h"

#define BILLBOARD_MAX_PROJECTED_OBJECTS 12
#define BILLBOARD_MAX_PROJECTED_EFFECTS 4
#define BILLBOARD_MAX_PROJECTED_TOTAL \
    (BILLBOARD_MAX_PROJECTED_OBJECTS + BILLBOARD_MAX_PROJECTED_EFFECTS)

// These values ARE atlas indices: visual_id indexes FREEDOOM_BILLBOARD_WORLD_
// TEXTURES and FREEDOOM_BILLBOARD_WORLD_GEOMETRY directly. They must therefore
// stay in the exact order of $BillboardWorldSpecs in
// tools/convert-freedoom-assets.ps1 -- tools/test-billboard-layout.py asserts
// that order. The collectibles come first, because $billboardPickupCount treats
// the front of the list as the sprites that get column-post tables.
typedef enum {
    BILLBOARD_VISUAL_BONUS = 0,
    BILLBOARD_VISUAL_BLUE_KEY = 1,
    BILLBOARD_VISUAL_YELLOW_KEY = 2,
    BILLBOARD_VISUAL_RED_KEY = 3,
    BILLBOARD_VISUAL_STIMPACK = 4,
    BILLBOARD_VISUAL_MEDIKIT = 5,
    BILLBOARD_VISUAL_ARMOR_BONUS = 6,
    BILLBOARD_VISUAL_GREEN_ARMOR = 7,
    BILLBOARD_VISUAL_BLUE_ARMOR = 8,
    BILLBOARD_VISUAL_CLIP = 9,
    BILLBOARD_VISUAL_AMMO_BOX = 10,
    BILLBOARD_VISUAL_SHELLS = 11,
    BILLBOARD_VISUAL_SHELL_BOX = 12,
    BILLBOARD_VISUAL_SHOTGUN_PICKUP = 13,
    BILLBOARD_VISUAL_CHAINGUN_PICKUP = 14,
    BILLBOARD_VISUAL_CHAINSAW_PICKUP = 15,
    BILLBOARD_VISUAL_CANDLE = 16,
    BILLBOARD_VISUAL_CANDELABRA = 17,
    BILLBOARD_VISUAL_COLUMN = 18,
    BILLBOARD_VISUAL_ELEC = 19,
    BILLBOARD_VISUAL_BARREL = 20,
    BILLBOARD_VISUAL_TREE = 21,
    // Past the world atlas: these draw from their own frame arrays.
    BILLBOARD_VISUAL_DUMMY = 22,
    BILLBOARD_VISUAL_DUMMY_DAMAGED = 23,
    BILLBOARD_VISUAL_BARREL_EXPLODING = 24,
    BILLBOARD_VISUAL_PUFF = 25,
    BILLBOARD_VISUAL_BLOOD = 26
} BillboardVisualId;

typedef struct {
    s16 left;
    s16 right;
    s16 top;
    s16 bottom;
    u16 depth;
    u8 visual_id;
    u8 frame;
    u8 atlas_x;
    u8 atlas_y;
    u8 atlas_w;
    u8 atlas_h;
} ProjectedBillboard;

typedef struct {
    u16 bonus;
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
    BILLBOARD_EFFECT_KEY = 4,
    BILLBOARD_EFFECT_WEAPON = 5
} BillboardEffect;

typedef struct {
    bool collected;
    u8 effect;
    u16 amount;
    u8 key_mask;
    BillboardPickupKind kind;
    // BILLBOARD_EFFECT_AMMO: which pool `amount` goes into (an AmmoType).
    // BILLBOARD_EFFECT_WEAPON: the WeaponId granted; `amount` is its free ammo
    // and ammo_type is the pool that ammo belongs to.
    u8 ammo_type;
    u8 weapon_id;
} BillboardPickupResult;

typedef enum {
    BILLBOARD_SHOT_NONE = 0,
    BILLBOARD_SHOT_DAMAGE = 1,
    BILLBOARD_SHOT_KILL = 2,
    BILLBOARD_SHOT_EXPLOSION = 3
} BillboardShotResult;

typedef struct {
    BillboardShotResult status;
    u16 player_damage;
    s16 push_x;
    s16 push_y;
    u8 explosion_count;
} BillboardFireResult;

typedef struct {
    bool moved;
    bool position_changed;
    bool pose_changed;
    u16 hits;
    s16 push_x;
    s16 push_y;
} BillboardEnemyUpdate;

void billboard_init(u16 phase_index, DoomSkill skill);
BillboardPickupResult billboard_collect_near(s32 x, s32 y);
u16 billboard_get_collected_count(void);
BillboardPickupCounts billboard_get_pickup_counts(void);
BillboardPickupKind billboard_get_last_pickup_kind(void);
u16 billboard_get_enemy_count(void);
u16 billboard_get_active_count(void);
u16 billboard_get_kill_total(void);
u16 billboard_get_kill_count(void);
u16 billboard_get_item_total(void);
u16 billboard_get_item_count(void);
u16 billboard_get_target_count(void);
u16 billboard_get_target_health(void);
// Fire one hitscan pellet down view column `aim_col`, blocked at `wall_depth`
// (the wall distance at that same column) and dealing `damage` Doom hit points.
// Multi-pellet weapons call this once per pellet and merge the results.
BillboardFireResult billboard_fire_center(const PlayerState *player, u16 wall_depth,
                                          s16 aim_col, u16 damage);
BillboardEnemyUpdate billboard_update_enemies(const PlayerState *player,
                                               bool redraw_pending);
BillboardEnemyUpdate billboard_update_barrels(const PlayerState *player);
bool billboard_update_effects(void);
bool billboard_position_blocked(s32 x, s32 y, s32 radius);
#if DEBUG_PERF
u16 billboard_get_debug_culled_count(void);
u16 billboard_get_debug_candidate_count(void);
u16 billboard_get_debug_occluded_count(void);
u16 billboard_get_debug_projected_count(void);
u16 billboard_get_debug_projection_cache_hits(void);
u16 billboard_get_debug_projection_cache_misses(void);
u16 billboard_get_debug_prop_collision_candidates(void);
u16 billboard_get_debug_prop_collision_calls(void);
u16 billboard_get_debug_prop_collision_scanned(void);
u16 billboard_get_debug_visibility_cache_hits(void);
u16 billboard_get_debug_visibility_cache_misses(void);
u16 billboard_get_debug_simulated_enemy_count(void);
u16 billboard_get_debug_enemy_pair_tests(void);
u16 billboard_get_debug_enemy_close_pairs(void);
u16 billboard_get_debug_enemy_separation_attempts(void);
u16 billboard_get_debug_enemy_separation_moves(void);
u32 billboard_get_debug_enemy_separation_subticks(void);
void billboard_debug_reset_stats(void);
#endif
u16 billboard_project_scene(const PlayerState *player,
                            const RayColumn *columns,
                            ProjectedBillboard *objects,
                            u16 max_objects);

#endif
