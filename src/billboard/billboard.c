#include "billboard_internal.h"
#include "billboard_effects.h"
#include "generated_billboard_assets.h"
#include "generated_billboard_geometry.h"
#include "billboard_projection_lut.h"
#include "renderer_perf.h"
#include "weapons.h"

// type: visual, effect, HP, radius, visual scale, max depth,
// collectible, targetable, blocking.
// Item/prop render geometry comes from the Doom patch metadata, not this table.
static const BillboardType BILLBOARD_TYPES[BILLBOARD_TYPE_COUNT] = {
    {BILLBOARD_VISUAL_BONUS,       BILLBOARD_EFFECT_HEALTH, 1, 20, BILLBOARD_PICKUP_VISUAL_SCALE, BILLBOARD_MAX_DEPTH, TRUE,  FALSE, FALSE},
    {BILLBOARD_VISUAL_BLUE_KEY,    BILLBOARD_EFFECT_KEY,    1, 20, BILLBOARD_PICKUP_VISUAL_SCALE, BILLBOARD_MAX_DEPTH, TRUE,  FALSE, FALSE},
    {BILLBOARD_VISUAL_STIMPACK,    BILLBOARD_EFFECT_HEALTH, 1, 20, BILLBOARD_PICKUP_VISUAL_SCALE, BILLBOARD_MAX_DEPTH, TRUE,  FALSE, FALSE},
    {BILLBOARD_VISUAL_MEDIKIT,     BILLBOARD_EFFECT_HEALTH, 1, 24, BILLBOARD_PICKUP_VISUAL_SCALE, BILLBOARD_MAX_DEPTH, TRUE,  FALSE, FALSE},
    {BILLBOARD_VISUAL_ARMOR_BONUS, BILLBOARD_EFFECT_ARMOR,  1, 16, BILLBOARD_PICKUP_VISUAL_SCALE, BILLBOARD_MAX_DEPTH, TRUE,  FALSE, FALSE},
    {BILLBOARD_VISUAL_GREEN_ARMOR, BILLBOARD_EFFECT_ARMOR,  1, 28, BILLBOARD_PICKUP_VISUAL_SCALE, BILLBOARD_MAX_DEPTH, TRUE,  FALSE, FALSE},
    {BILLBOARD_VISUAL_BLUE_ARMOR,  BILLBOARD_EFFECT_ARMOR,  1, 28, BILLBOARD_PICKUP_VISUAL_SCALE, BILLBOARD_MAX_DEPTH, TRUE,  FALSE, FALSE},
    {BILLBOARD_VISUAL_CLIP,        BILLBOARD_EFFECT_AMMO,   1, 18, BILLBOARD_PICKUP_VISUAL_SCALE, BILLBOARD_MAX_DEPTH, TRUE,  FALSE, FALSE},
    {BILLBOARD_VISUAL_AMMO_BOX,    BILLBOARD_EFFECT_AMMO,   1, 24, BILLBOARD_PICKUP_VISUAL_SCALE, BILLBOARD_MAX_DEPTH, TRUE,  FALSE, FALSE},
    {BILLBOARD_VISUAL_CANDLE,      BILLBOARD_EFFECT_NONE,   1, BILLBOARD_PROP_RADIUS, 1, BILLBOARD_MAX_DEPTH, FALSE, FALSE, TRUE},
    {BILLBOARD_VISUAL_CANDELABRA,  BILLBOARD_EFFECT_NONE,   1, BILLBOARD_PROP_RADIUS, 1, BILLBOARD_MAX_DEPTH, FALSE, FALSE, TRUE},
    {BILLBOARD_VISUAL_COLUMN,      BILLBOARD_EFFECT_NONE,   1, 16, 1, BILLBOARD_MAX_DEPTH, FALSE, FALSE, TRUE},
    {BILLBOARD_VISUAL_ELEC,        BILLBOARD_EFFECT_NONE,   1, 16, 1, BILLBOARD_MAX_DEPTH, FALSE, FALSE, TRUE},
    {BILLBOARD_VISUAL_BARREL,      BILLBOARD_EFFECT_NONE,   1, 20, BILLBOARD_BARREL_VISUAL_SCALE, BILLBOARD_MAX_DEPTH, FALSE, TRUE,  TRUE},
    {BILLBOARD_VISUAL_TREE,        BILLBOARD_EFFECT_NONE,   1, 48, 1, BILLBOARD_MAX_DEPTH, FALSE, FALSE, TRUE},
    // Doom hit points, not "shots to kill": billboard_fire_center now takes a
    // per-weapon damage amount (weapon_roll_damage, 5/10/15 per bullet), so a
    // zombieman-class enemy at Doom's 20 HP still dies in about two pistol hits
    // while a shotgun blast drops it outright. The barrel matches Doom's 20 too.
    {BILLBOARD_VISUAL_DUMMY,       BILLBOARD_EFFECT_NONE,   20, 24, 1, BILLBOARD_MAX_DEPTH, FALSE, TRUE,  FALSE},
    {BILLBOARD_VISUAL_SHELLS,      BILLBOARD_EFFECT_AMMO,   1, 16, BILLBOARD_PICKUP_VISUAL_SCALE, BILLBOARD_MAX_DEPTH, TRUE,  FALSE, FALSE},
    {BILLBOARD_VISUAL_SHELL_BOX,   BILLBOARD_EFFECT_AMMO,   1, 24, BILLBOARD_PICKUP_VISUAL_SCALE, BILLBOARD_MAX_DEPTH, TRUE,  FALSE, FALSE},
    {BILLBOARD_VISUAL_SHOTGUN_PICKUP,  BILLBOARD_EFFECT_WEAPON, 1, 24, BILLBOARD_PICKUP_VISUAL_SCALE, BILLBOARD_MAX_DEPTH, TRUE, FALSE, FALSE},
    {BILLBOARD_VISUAL_CHAINGUN_PICKUP, BILLBOARD_EFFECT_WEAPON, 1, 24, BILLBOARD_PICKUP_VISUAL_SCALE, BILLBOARD_MAX_DEPTH, TRUE, FALSE, FALSE},
    {BILLBOARD_VISUAL_CHAINSAW_PICKUP, BILLBOARD_EFFECT_WEAPON, 1, 24, BILLBOARD_PICKUP_VISUAL_SCALE, BILLBOARD_MAX_DEPTH, TRUE, FALSE, FALSE},
};

BillboardObject g_billboards[BILLBOARD_OBJECT_COUNT];
u16 g_collected_count;
BillboardPickupCounts g_pickup_counts;
BillboardPickupKind g_last_pickup_kind;
static u16 g_visibility_generation = 1;
static u16 g_visibility_entry_generation[BILLBOARD_OBJECT_COUNT];
static bool g_visibility_result[BILLBOARD_OBJECT_COUNT];
static s32 g_visibility_player_x;
static s32 g_visibility_player_y;
static u16 g_visibility_bsp_revision;
static bool g_visibility_context_valid;
static u16 g_level_kill_total;
static u16 g_level_item_total;
static u16 g_level_item_count;
#if DEBUG_PERF
static u16 g_debug_prop_collision_candidates;
static u16 g_debug_prop_collision_calls;
static u16 g_debug_prop_collision_scanned;
static u16 g_debug_visibility_cache_hits;
static u16 g_debug_visibility_cache_misses;
#endif

#define DOOM_THING_SKILL_EASY 0x0001u
#define DOOM_THING_SKILL_MEDIUM 0x0002u
#define DOOM_THING_SKILL_HARD 0x0004u
#define DOOM_THING_NOT_SINGLE_PLAYER 0x0010u

static u16 doom_skill_thing_mask(DoomSkill skill) {
    if (skill <= DOOM_SKILL_HEY_NOT_TOO_ROUGH) return DOOM_THING_SKILL_EASY;
    if (skill == DOOM_SKILL_HURT_ME_PLENTY) return DOOM_THING_SKILL_MEDIUM;
    return DOOM_THING_SKILL_HARD;
}

static u8 map_thing_type(u16 doom_type, u8 *visual) {
    switch (doom_type) {
        case 5:  *visual = BILLBOARD_VISUAL_BLUE_KEY; return BILLBOARD_TYPE_KEY;
        case 6:  *visual = BILLBOARD_VISUAL_YELLOW_KEY; return BILLBOARD_TYPE_KEY;
        case 13: *visual = BILLBOARD_VISUAL_RED_KEY; return BILLBOARD_TYPE_KEY;
        case 2014: *visual = BILLBOARD_VISUAL_BONUS; return BILLBOARD_TYPE_BONUS;
        case 2015: *visual = BILLBOARD_VISUAL_ARMOR_BONUS; return BILLBOARD_TYPE_ARMOR_BONUS;
        case 2011: *visual = BILLBOARD_VISUAL_STIMPACK; return BILLBOARD_TYPE_STIMPACK;
        case 2012: *visual = BILLBOARD_VISUAL_MEDIKIT; return BILLBOARD_TYPE_MEDIKIT;
        case 2018: *visual = BILLBOARD_VISUAL_GREEN_ARMOR; return BILLBOARD_TYPE_GREEN_ARMOR;
        case 2019: *visual = BILLBOARD_VISUAL_BLUE_ARMOR; return BILLBOARD_TYPE_BLUE_ARMOR;
        case 2007: *visual = BILLBOARD_VISUAL_CLIP; return BILLBOARD_TYPE_CLIP;
        case 2048: *visual = BILLBOARD_VISUAL_AMMO_BOX; return BILLBOARD_TYPE_AMMO_BOX;
        case 2008: *visual = BILLBOARD_VISUAL_SHELLS; return BILLBOARD_TYPE_SHELLS;
        case 2049: *visual = BILLBOARD_VISUAL_SHELL_BOX; return BILLBOARD_TYPE_SHELL_BOX;
        case 2001: *visual = BILLBOARD_VISUAL_SHOTGUN_PICKUP; return BILLBOARD_TYPE_SHOTGUN;
        case 2002: *visual = BILLBOARD_VISUAL_CHAINGUN_PICKUP; return BILLBOARD_TYPE_CHAINGUN;
        case 2005: *visual = BILLBOARD_VISUAL_CHAINSAW_PICKUP; return BILLBOARD_TYPE_CHAINSAW;
        // Decorative THINGS are deliberately omitted on the Mega Drive. They
        // consume projection, LOS, draw and collision time without gameplay.
        case 2035: *visual = BILLBOARD_VISUAL_BARREL; return BILLBOARD_TYPE_BARREL;
        case 9: case 3001: case 3004: *visual = BILLBOARD_VISUAL_DUMMY; return BILLBOARD_TYPE_DUMMY;
        default: return 0xFF;
    }
}

const BillboardType *billboard_get_type(u8 type_id) {
    return (type_id < BILLBOARD_TYPE_COUNT) ? &BILLBOARD_TYPES[type_id] : &BILLBOARD_TYPES[BILLBOARD_TYPE_BONUS];
}

u8 billboard_get_object_visual_id(const BillboardObject *object, const BillboardType *type) {
    (void)type;
    if (object->type_id == BILLBOARD_TYPE_KEY) return object->hp;
    if ((object->type_id == BILLBOARD_TYPE_BARREL) && (object->life_state != ENEMY_ALIVE)) {
        return BILLBOARD_VISUAL_BARREL_EXPLODING;
    }
    return type->visual_id;
}

u8 billboard_get_object_frame(const BillboardObject *object) {
    if (object->type_id == BILLBOARD_TYPE_BARREL) {
        if (object->life_state == ENEMY_DYING) {
            u8 index = object->death_index;
            if (index >= BARREL_DEATH_FRAME_COUNT) index = (u8)(BARREL_DEATH_FRAME_COUNT - 1);
            return index;
        }
        return 0;
    }
    if (object->type_id != BILLBOARD_TYPE_DUMMY) return 0;
    if (object->life_state != ENEMY_ALIVE) {
        u8 index = object->death_index;
        if (index >= ENEMY_DEATH_FRAME_COUNT) index = (u8)(ENEMY_DEATH_FRAME_COUNT - 1);
        return (u8)(ENEMY_DEATH_FRAME_BASE + index);
    }
    if (object->attack_anim > 0) return ENEMY_ATTACK_FRAME_INDEX;
    return (u8)(object->anim_frame & (ENEMY_WALK_FRAME_COUNT - 1));
}

static s32 bb_divs(s32 numerator, s32 denominator) {
    // Active E1M1 THINGs and the collision-constrained player keep this
    // projection inside the signed-word quotient domain.
    return (s32)divs(numerator, (s16)denominator);
}

// K/forward via a precomputed table (see billboard_projection_lut.h for the
// [33,1535] range proof); `numerator` is only used by the fallback path for
// any forward this call site's proof didn't already cover.
static u16 bb_lut_divu(const u16 *table, u32 numerator, u16 forward) {
    u16 result;
    bool fallback;
    if (forward >= 1 && forward <= 1535) {
        result = table[forward - 1];
        fallback = FALSE;
    } else {
        result = divu(numerator, forward);
        fallback = TRUE;
    }
#if DEBUG_PERF
    renderer_perf_record_billboard_lut(forward, fallback);
#endif
    return result;
}

#define BILLBOARD_SCALE_SHIFT 12
// WAD patch geometry is projected in native Doom map units
// (BILLBOARD_WORLD_GEOMETRY_SCALE == 1, matching RAY_WORLD_WALL_HEIGHT's
// 128-unit wall baseline) and then enlarged per type via
// BillboardType.visual_scale: pickups read too small on screen at native
// size, so they use a 3x visual_scale (BILLBOARD_PICKUP_VISUAL_SCALE);
// barrels use a smaller 2x visual_scale (BILLBOARD_BARREL_VISUAL_SCALE);
// decor props stay at 1x. See billboard_internal.h.
static s32 billboard_muls_word(s16 left, s16 right) {
    s32 result = left;
    __asm__ volatile (
        "muls.w %1,%0"
        : "+d" (result)
        : "d" (right)
        : "cc");
    return result;
}

static s32 billboard_mul_basis_delta(s16 basis, s32 delta) {
    return billboard_muls_word(basis, (s16)delta);
}

static s16 billboard_project_q12(s16 value, u16 scale_q12) {
    s32 product = billboard_muls_word(value, (s16)scale_q12);
    if (product < 0) return (s16)-((-product) >> BILLBOARD_SCALE_SHIFT);
    return (s16)(product >> BILLBOARD_SCALE_SHIFT);
}

static s16 billboard_project_world_q12(s16 value, u16 scale_q12,
                                       u8 visual_scale) {
    const s16 scaled = (s16)(value * BILLBOARD_WORLD_GEOMETRY_SCALE * visual_scale);
    return billboard_project_q12(scaled, scale_q12);
}

static void billboard_get_geometry(const BillboardObject *object,
                                   const BillboardType *type,
                                   BillboardGeometry *geometry) {
    u8 visual_id = billboard_get_object_visual_id(object, type);
    if (visual_id == BILLBOARD_VISUAL_BARREL_EXPLODING) {
        const u8 frame = billboard_get_object_frame(object);
        const s16 *source = FREEDOOM_BILLBOARD_BARREL_EXPLOSION_GEOMETRY[frame];
        geometry->source_w = source[0];
        geometry->source_h = source[1];
        geometry->left_offset = source[2];
        geometry->top_offset = source[3];
        geometry->atlas_x = 0;
        geometry->atlas_y = 0;
        geometry->atlas_w = (u8)source[0];
        geometry->atlas_h = (u8)source[1];
        return;
    }
    if (visual_id >= FREEDOOM_BILLBOARD_WORLD_GEOMETRY_COUNT) {
        visual_id = type->visual_id;
    }
    if (visual_id < FREEDOOM_BILLBOARD_WORLD_GEOMETRY_COUNT) {
        const s16 *source = FREEDOOM_BILLBOARD_WORLD_GEOMETRY[visual_id];
        geometry->source_w = source[FREEDOOM_BILLBOARD_GEOMETRY_SOURCE_W];
        geometry->source_h = source[FREEDOOM_BILLBOARD_GEOMETRY_SOURCE_H];
        geometry->left_offset = source[FREEDOOM_BILLBOARD_GEOMETRY_LEFT_OFFSET];
        geometry->top_offset = source[FREEDOOM_BILLBOARD_GEOMETRY_TOP_OFFSET];
        geometry->atlas_x = (u8)source[FREEDOOM_BILLBOARD_GEOMETRY_ATLAS_X];
        geometry->atlas_y = (u8)source[FREEDOOM_BILLBOARD_GEOMETRY_ATLAS_Y];
        geometry->atlas_w = (u8)source[FREEDOOM_BILLBOARD_GEOMETRY_ATLAS_W];
        geometry->atlas_h = (u8)source[FREEDOOM_BILLBOARD_GEOMETRY_ATLAS_H];
        return;
    }

    // Enemy poses carry their own world box (ENEMY_FRAME_GEOMETRY), already
    // expressed in world units, so they ride the same WAD-origin projection as
    // every other billboard. The atlas sub-rect stays the full 24x48 cell: each
    // cell is its whole source patch stretched to fill, so scaling that cell to
    // the pose's own box reproduces the patch at the right size and height.
    {
        const u8 frame = billboard_get_object_frame(object);
        const s16 *source =
            ENEMY_FRAME_GEOMETRY[(frame < ENEMY_FRAME_GEOMETRY_COUNT) ? frame : 0];
        geometry->source_w = source[0];
        geometry->source_h = source[1];
        geometry->left_offset = source[2];
        geometry->top_offset = source[3];
    }
    geometry->atlas_x = 0;
    geometry->atlas_y = 0;
    geometry->atlas_w = BILLBOARD_ENEMY_ATLAS_WIDTH;
    geometry->atlas_h = BILLBOARD_ENEMY_ATLAS_HEIGHT;
}

bool billboard_measure_object(const PlayerState *player, s16 cos_a, s16 sin_a,
                              const BillboardObject *object, BillboardMeasure *measure) {
    if (!object->active) return FALSE;
    const BillboardType *type = billboard_get_type(object->type_id);
    const s32 dx = object->x - player->x;
    const s32 dy = object->y - player->y;
    const s32 forward = (billboard_mul_basis_delta(cos_a, dx) +
                         billboard_mul_basis_delta(sin_a, dy)) >> FX_SHIFT;
    // Depth-reject before computing `side`. Projection re-measures every active
    // registry object whenever the player moves or turns, and most of them fail
    // right here, so the two extra 32x16 multiplies `side` needs are pure waste
    // on the common path. Nothing between here and its first use reads it.
    if ((forward <= BILLBOARD_MIN_DEPTH) || (forward >= type->max_depth)) return FALSE;
    const s32 side = (billboard_mul_basis_delta(cos_a, dy) -
                      billboard_mul_basis_delta(sin_a, dx)) >> FX_SHIFT;
    BillboardGeometry geometry;
    billboard_get_geometry(object, type, &geometry);

    // Reject a sprite whose complete authored horizontal span is safely beyond
    // the viewport before paying for centre/scale divisions. The two-pixel
    // expansion covers the separate truncation of centre and edge extents in
    // the exact projector below, so an edge-touching sprite is never lost.
    {
        const s32 geometry_scale =
            BILLBOARD_WORLD_GEOMETRY_SCALE * type->visual_scale;
        const s32 left_extent = (s32)geometry.left_offset * geometry_scale;
        const s32 right_extent =
            ((s32)geometry.source_w - geometry.left_offset) * geometry_scale;
        const s32 left_numerator = (side - left_extent) * RAY_PROJ_X;
        const s32 right_numerator = (side + right_extent) * RAY_PROJ_X;
        const s32 left_clip = -((s32)(RAY_VIEW_CENTER_X + 2) * forward);
        const s32 right_clip =
            (s32)(RAY_VIEW_COLS - RAY_VIEW_CENTER_X + 2) * forward;
        if ((right_numerator < left_clip) || (left_numerator >= right_clip)) {
            return FALSE;
        }
    }

    measure->type = type;
    measure->forward = forward;
    measure->side = side;
    measure->center_col = (s16)(RAY_VIEW_CENTER_X + bb_divs(side * RAY_PROJ_X, forward));
    measure->atlas_x = geometry.atlas_x;
    measure->atlas_y = geometry.atlas_y;
    measure->atlas_w = geometry.atlas_w;
    measure->atlas_h = geometry.atlas_h;

    {
        // Q12 screen scale via a precomputed table (both axes share the same
        // K == RAY_PROJ_X<<12 == RAY_PROJ_Y<<12, since RAY_PROJ_X == RAY_PROJ_Y).
        // All patch edges then use MULS.W + shifts, avoiding any extra divisions.
        const u16 scale_x_q12 = bb_lut_divu(g_billboard_recip_proj_lut,
            (u32)RAY_PROJ_X << BILLBOARD_SCALE_SHIFT, (u16)forward);
        const u16 scale_y_q12 = bb_lut_divu(g_billboard_recip_proj_lut,
            (u32)RAY_PROJ_Y << BILLBOARD_SCALE_SHIFT, (u16)forward);
        const s16 origin_y = (s16)(RAY_VIEW_CENTER_Y +
            billboard_project_q12(RAY_CAMERA_HEIGHT, scale_y_q12));
        const s16 right_extent = billboard_project_world_q12(
            (s16)(geometry.source_w - geometry.left_offset), scale_x_q12,
            type->visual_scale);
        // Sprite pixels and walls share one focal scale on both axes.
        const s16 bottom_extent = billboard_project_world_q12(
            (s16)(geometry.source_h - geometry.top_offset), scale_x_q12,
            type->visual_scale);

        measure->left = (s16)(measure->center_col -
            billboard_project_world_q12(geometry.left_offset, scale_x_q12,
                                        type->visual_scale));
        measure->right = (s16)(measure->center_col + right_extent - 1);
        measure->top = (s16)(origin_y -
            billboard_project_world_q12(geometry.top_offset, scale_x_q12,
                                        type->visual_scale));
        measure->bottom = (s16)(origin_y + bottom_extent - 1);
        if (measure->right < measure->left) measure->right = measure->left;
        if (measure->bottom < measure->top) measure->bottom = measure->top;
        measure->half_w = (s16)(measure->center_col - measure->left);
        if ((measure->right - measure->center_col) > measure->half_w) {
            measure->half_w = (s16)(measure->right - measure->center_col);
        }
        measure->projected_height = (s16)(measure->bottom - measure->top + 1);
    }
    return TRUE;
}

void billboard_init(u16 phase_index, DoomSkill skill) {
    u16 count = 0;
    const u16 thing_skill_mask = doom_skill_thing_mask(skill);
    (void)phase_index;
    billboard_registry_reset();
    billboard_effects_reset();
    g_level_kill_total = 0;
    g_level_item_total = 0;
    g_level_item_count = 0;
    for (u16 i = 0; i < BILLBOARD_OBJECT_COUNT; i++) g_billboards[i].active = FALSE;
    for (u16 i = 0; i < bsp_thing_count && count < BILLBOARD_OBJECT_COUNT; i++) {
        const u16 flags = bsp_things[i].flags;
        u8 visual = 0;

        // The generated map retains the original Doom skill bits. Five menu
        // choices map to Doom's three easy/normal/hard THING populations.
        if ((flags & thing_skill_mask) == 0 ||
            (flags & DOOM_THING_NOT_SINGLE_PLAYER) != 0) {
            continue;
        }
        const u8 type = map_thing_type(bsp_things[i].type, &visual);
        if (type == 0xFF) continue;
        const u16 object_index = count++;
        BillboardObject *object = &g_billboards[object_index];
        *object = (BillboardObject){0};
        object->x = bsp_things[i].x;
        object->y = bsp_things[i].y;
        object->type_id = type;
        object->hp = visual; // key color is carried as its visual ID.
        object->active = TRUE;
        object->home_x = object->x;
        object->home_y = object->y;
        if (type != BILLBOARD_TYPE_KEY) object->hp = billboard_get_type(type)->hit_points;
        billboard_registry_add(object_index);
        if (type == BILLBOARD_TYPE_DUMMY) g_level_kill_total++;
        if ((type == BILLBOARD_TYPE_BONUS) ||
            (type == BILLBOARD_TYPE_ARMOR_BONUS)) g_level_item_total++;
    }
    g_collected_count = 0;
    g_pickup_counts.bonus = 0;
    g_last_pickup_kind = BILLBOARD_PICKUP_NONE;
    g_visibility_context_valid = FALSE;
    g_visibility_generation = 1;
    for (u16 i = 0; i < BILLBOARD_OBJECT_COUNT; i++) {
        g_visibility_entry_generation[i] = 0;
    }
}

void billboard_visibility_begin(const PlayerState *player) {
    const u16 bsp_revision = bsp_get_visibility_revision();

    if (g_visibility_context_valid &&
        g_visibility_player_x == player->x &&
        g_visibility_player_y == player->y &&
        g_visibility_bsp_revision == bsp_revision) {
        return;
    }

    g_visibility_player_x = player->x;
    g_visibility_player_y = player->y;
    g_visibility_bsp_revision = bsp_revision;
    g_visibility_context_valid = TRUE;
    g_visibility_generation++;
    if (g_visibility_generation == 0) {
        for (u16 i = 0; i < BILLBOARD_OBJECT_COUNT; i++) {
            g_visibility_entry_generation[i] = 0;
        }
        g_visibility_generation = 1;
    }
}

bool billboard_has_line_of_sight(u16 index, const PlayerState *player) {
    billboard_visibility_begin(player);
    if (index >= BILLBOARD_OBJECT_COUNT) return FALSE;

    if (g_visibility_entry_generation[index] == g_visibility_generation) {
#if DEBUG_PERF
        g_debug_visibility_cache_hits++;
#endif
        return g_visibility_result[index];
    }

#if DEBUG_PERF
    g_debug_visibility_cache_misses++;
#endif
    g_visibility_result[index] = !bsp_segment_hits_wall(
        g_billboards[index].x, g_billboards[index].y, player->x, player->y);
    g_visibility_entry_generation[index] = g_visibility_generation;
    return g_visibility_result[index];
}

void billboard_invalidate_object_visibility(u16 index) {
    if (index < BILLBOARD_OBJECT_COUNT) {
        g_visibility_entry_generation[index] = 0;
    }
}

// WeaponId of the weapon each weapon-pickup type grants. Kept here rather than
// in BillboardType so the billboard module does not need the weapon table.
static u8 pickup_weapon_id(u8 type_id) {
    switch (type_id) {
        case BILLBOARD_TYPE_SHOTGUN:  return WEAPON_SHOTGUN;
        case BILLBOARD_TYPE_CHAINGUN: return WEAPON_CHAINGUN;
        default:                      return WEAPON_CHAINSAW;
    }
}

BillboardPickupResult billboard_collect_near(s32 x, s32 y) {
    BillboardPickupResult result = {FALSE, BILLBOARD_EFFECT_NONE, 0, BSP_KEY_NONE,
                                    BILLBOARD_PICKUP_NONE, AMMO_NONE, WEAPON_FIST};
    const u8 *indices = billboard_registry_active_indices();
    const u16 active_count = billboard_registry_active_count();
    for (u16 slot = 0; slot < active_count; slot++) {
        const u16 i = indices[slot];
        BillboardObject *object = &g_billboards[i];
        const BillboardType *type = billboard_get_type(object->type_id);
        if (!object->active || !type->collectible) continue;
        const s32 dx = object->x - x;
        const s32 dy = object->y - y;
        if ((dx * dx) + (dy * dy) > BILLBOARD_COLLECT_RADIUS_SQ) continue;
        billboard_registry_deactivate(i);
        result.collected = TRUE;
        result.effect = type->effect;
        if (type->effect == BILLBOARD_EFFECT_HEALTH) {
            result.amount = (object->type_id == BILLBOARD_TYPE_MEDIKIT) ? 25 :
                            ((object->type_id == BILLBOARD_TYPE_BONUS) ? 1 : 10);
        } else if (type->effect == BILLBOARD_EFFECT_ARMOR) {
            result.amount = (object->type_id == BILLBOARD_TYPE_BLUE_ARMOR) ? 200 :
                            ((object->type_id == BILLBOARD_TYPE_ARMOR_BONUS) ? 1 : 100);
        }
        else if (type->effect == BILLBOARD_EFFECT_AMMO) {
            // Doom's pickup amounts: clip 10, box of bullets 50, 4 shells,
            // 20 shells in a box.
            switch (object->type_id) {
                case BILLBOARD_TYPE_AMMO_BOX:
                    result.amount = 50; result.ammo_type = AMMO_BULLETS; break;
                case BILLBOARD_TYPE_SHELLS:
                    result.amount = 4;  result.ammo_type = AMMO_SHELLS;  break;
                case BILLBOARD_TYPE_SHELL_BOX:
                    result.amount = 20; result.ammo_type = AMMO_SHELLS;  break;
                default:
                    result.amount = 10; result.ammo_type = AMMO_BULLETS; break;
            }
        }
        else if (type->effect == BILLBOARD_EFFECT_WEAPON) {
            result.weapon_id = pickup_weapon_id(object->type_id);
            result.amount = WEAPON_PICKUP_AMMO[result.weapon_id];
            result.ammo_type = WEAPON_DEFS[result.weapon_id].ammo_type;
        }
        else if (type->effect == BILLBOARD_EFFECT_KEY) {
            if (object->hp == BILLBOARD_VISUAL_BLUE_KEY) result.key_mask = BSP_KEY_BLUE;
            else if (object->hp == BILLBOARD_VISUAL_YELLOW_KEY) result.key_mask = BSP_KEY_YELLOW;
            else if (object->hp == BILLBOARD_VISUAL_RED_KEY) result.key_mask = BSP_KEY_RED;
            result.kind = BILLBOARD_PICKUP_KEY;
        } else { result.kind = BILLBOARD_PICKUP_BONUS; g_pickup_counts.bonus++; }
        if ((object->type_id == BILLBOARD_TYPE_BONUS) ||
            (object->type_id == BILLBOARD_TYPE_ARMOR_BONUS)) {
            g_level_item_count++;
        }
        g_collected_count++;
        g_last_pickup_kind = result.kind;
        return result;
    }
    return result;
}

bool billboard_position_blocked(s32 x, s32 y, s32 radius) {
    const u8 *indices = billboard_registry_blocking_indices();
    const u16 blocking_count = billboard_registry_blocking_count();
#if DEBUG_PERF
    g_debug_prop_collision_calls++;
#endif
    for (u16 slot = 0; slot < blocking_count; slot++) {
#if DEBUG_PERF
        g_debug_prop_collision_scanned++;
#endif
        const u16 i = indices[slot];
        const BillboardObject *object = &g_billboards[i];
        const BillboardType *type = billboard_get_type(object->type_id);
        if (!object->active || !type->blocking) continue;
#if DEBUG_PERF
        g_debug_prop_collision_candidates++;
#endif
        const s32 dx = object->x - x;
        const s32 dy = object->y - y;
        const s32 limit = radius + type->radius;
        if ((dx * dx) + (dy * dy) < limit * limit) return TRUE;
    }
    return FALSE;
}

u16 billboard_get_collected_count(void) { return g_collected_count; }
u16 billboard_get_kill_total(void) { return g_level_kill_total; }
u16 billboard_get_kill_count(void) {
    return (u16)(g_level_kill_total - billboard_registry_living_enemy_count());
}
u16 billboard_get_item_total(void) { return g_level_item_total; }
u16 billboard_get_item_count(void) { return g_level_item_count; }
BillboardPickupCounts billboard_get_pickup_counts(void) { return g_pickup_counts; }
BillboardPickupKind billboard_get_last_pickup_kind(void) { return g_last_pickup_kind; }
u16 billboard_get_enemy_count(void) { return billboard_registry_living_enemy_count(); }
u16 billboard_get_active_count(void) { return billboard_registry_active_count(); }
#if DEBUG_PERF
u16 billboard_get_debug_prop_collision_candidates(void) { return g_debug_prop_collision_candidates; }
u16 billboard_get_debug_prop_collision_calls(void) { return g_debug_prop_collision_calls; }
u16 billboard_get_debug_prop_collision_scanned(void) { return g_debug_prop_collision_scanned; }
u16 billboard_get_debug_visibility_cache_hits(void) { return g_debug_visibility_cache_hits; }
u16 billboard_get_debug_visibility_cache_misses(void) { return g_debug_visibility_cache_misses; }
void billboard_debug_reset_stats(void) {
    g_debug_prop_collision_candidates = 0;
    g_debug_prop_collision_calls = 0;
    g_debug_prop_collision_scanned = 0;
    g_debug_visibility_cache_hits = 0;
    g_debug_visibility_cache_misses = 0;
}
#endif
