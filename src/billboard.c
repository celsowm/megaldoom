#include "billboard_internal.h"
#include "generated_billboard_geometry.h"

// type: visual, effect, HP, radius, max depth, collectible, targetable, blocking.
// Item/prop render geometry comes from the Doom patch metadata, not this table.
static const BillboardType BILLBOARD_TYPES[BILLBOARD_TYPE_COUNT] = {
    {BILLBOARD_VISUAL_BONUS,       BILLBOARD_EFFECT_HEALTH, 1, 20, BILLBOARD_MAX_DEPTH, TRUE,  FALSE, FALSE},
    {BILLBOARD_VISUAL_BLUE_KEY,    BILLBOARD_EFFECT_KEY,    1, 20, BILLBOARD_MAX_DEPTH, TRUE,  FALSE, FALSE},
    {BILLBOARD_VISUAL_STIMPACK,    BILLBOARD_EFFECT_HEALTH, 1, 20, BILLBOARD_MAX_DEPTH, TRUE,  FALSE, FALSE},
    {BILLBOARD_VISUAL_MEDIKIT,     BILLBOARD_EFFECT_HEALTH, 1, 24, BILLBOARD_MAX_DEPTH, TRUE,  FALSE, FALSE},
    {BILLBOARD_VISUAL_ARMOR_BONUS, BILLBOARD_EFFECT_ARMOR,  1, 16, BILLBOARD_MAX_DEPTH, TRUE,  FALSE, FALSE},
    {BILLBOARD_VISUAL_GREEN_ARMOR, BILLBOARD_EFFECT_ARMOR,  1, 28, BILLBOARD_MAX_DEPTH, TRUE,  FALSE, FALSE},
    {BILLBOARD_VISUAL_BLUE_ARMOR,  BILLBOARD_EFFECT_ARMOR,  1, 28, BILLBOARD_MAX_DEPTH, TRUE,  FALSE, FALSE},
    {BILLBOARD_VISUAL_CLIP,        BILLBOARD_EFFECT_AMMO,   1, 18, BILLBOARD_MAX_DEPTH, TRUE,  FALSE, FALSE},
    {BILLBOARD_VISUAL_AMMO_BOX,    BILLBOARD_EFFECT_AMMO,   1, 24, BILLBOARD_MAX_DEPTH, TRUE,  FALSE, FALSE},
    {BILLBOARD_VISUAL_CANDLE,      BILLBOARD_EFFECT_NONE,   1, BILLBOARD_PROP_RADIUS, BILLBOARD_MAX_DEPTH, FALSE, FALSE, TRUE},
    {BILLBOARD_VISUAL_CANDELABRA,  BILLBOARD_EFFECT_NONE,   1, BILLBOARD_PROP_RADIUS, BILLBOARD_MAX_DEPTH, FALSE, FALSE, TRUE},
    {BILLBOARD_VISUAL_COLUMN,      BILLBOARD_EFFECT_NONE,   1, 16, BILLBOARD_MAX_DEPTH, FALSE, FALSE, TRUE},
    {BILLBOARD_VISUAL_ELEC,        BILLBOARD_EFFECT_NONE,   1, 16, BILLBOARD_MAX_DEPTH, FALSE, FALSE, TRUE},
    {BILLBOARD_VISUAL_BARREL,      BILLBOARD_EFFECT_NONE,   1, 20, BILLBOARD_MAX_DEPTH, FALSE, FALSE, TRUE},
    {BILLBOARD_VISUAL_TREE,        BILLBOARD_EFFECT_NONE,   1, 48, BILLBOARD_MAX_DEPTH, FALSE, FALSE, TRUE},
    {BILLBOARD_VISUAL_DUMMY,       BILLBOARD_EFFECT_NONE,   3, 24, BILLBOARD_MAX_DEPTH, FALSE, TRUE,  FALSE},
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
#if DEBUG_PERF
static u16 g_debug_prop_collision_candidates;
static u16 g_debug_visibility_cache_hits;
static u16 g_debug_visibility_cache_misses;
#endif

#define DOOM_THING_SKILL_MEDIUM 0x0002u
#define DOOM_THING_NOT_SINGLE_PLAYER 0x0010u

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
    return object->type_id == BILLBOARD_TYPE_KEY ? object->hp : type->visual_id;
}

u8 billboard_get_object_frame(const BillboardObject *object) {
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
    if ((denominator > 0) && (denominator <= 0x7FFF)) {
        const s32 min = -((s32)denominator << 15);
        const s32 max = ((s32)denominator << 15) - denominator;
        if ((numerator >= min) && (numerator <= max)) return (s32)divs(numerator, (s16)denominator);
    }
    return numerator / denominator;
}

#define BILLBOARD_SCALE_SHIFT 12
// BSP walls use a 256-unit visual height while Doom's authored wall/sprite
// baseline is 128 units. Double WAD patch geometry so props retain their native
// proportions without appearing half-sized against the rendered architecture.
// BSP walls use a 256-unit visual height while Doom's authored wall/sprite
// baseline is 128 units, so patch geometry is first doubled (native 2x match).
// A further 1.5x growth makes barrels and pickups read larger on screen
// without touching enemies or walls. Effective scale is 2 * 1.5 = 3.
#define BILLBOARD_WORLD_GEOMETRY_SCALE 3

static s32 billboard_muls_word(s16 left, s16 right) {
    s32 result = left;
    __asm__ volatile (
        "muls.w %1,%0"
        : "+d" (result)
        : "d" (right)
        : "cc");
    return result;
}

static s16 billboard_project_q12(s16 value, u16 scale_q12) {
    s32 product = billboard_muls_word(value, (s16)scale_q12);
    if (product < 0) return (s16)-((-product) >> BILLBOARD_SCALE_SHIFT);
    return (s16)(product >> BILLBOARD_SCALE_SHIFT);
}

static s16 billboard_project_world_q12(s16 value, u16 scale_q12) {
    return billboard_project_q12((s16)(value * BILLBOARD_WORLD_GEOMETRY_SCALE), scale_q12);
}

static void billboard_get_geometry(const BillboardObject *object,
                                   const BillboardType *type,
                                   BillboardGeometry *geometry) {
    const u8 visual_id = billboard_get_object_visual_id(object, type);
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
        geometry->uses_wad_origin = TRUE;
        return;
    }

    // Preserve the established enemy size and bottom-centred anchor. This pass
    // intentionally changes only static world items and props.
    geometry->source_w = BILLBOARD_ENEMY_WORLD_WIDTH;
    geometry->source_h = BILLBOARD_ENEMY_WORLD_HEIGHT;
    geometry->left_offset = BILLBOARD_ENEMY_WORLD_WIDTH / 2;
    geometry->top_offset = BILLBOARD_ENEMY_WORLD_HEIGHT;
    geometry->atlas_x = 0;
    geometry->atlas_y = 0;
    geometry->atlas_w = BILLBOARD_ENEMY_ATLAS_WIDTH;
    geometry->atlas_h = BILLBOARD_ENEMY_ATLAS_HEIGHT;
    geometry->uses_wad_origin = FALSE;
}

bool billboard_measure_object(const PlayerState *player, s16 cos_a, s16 sin_a,
                              const BillboardObject *object, BillboardMeasure *measure) {
    if (!object->active) return FALSE;
    const BillboardType *type = billboard_get_type(object->type_id);
    const s32 dx = object->x - player->x;
    const s32 dy = object->y - player->y;
    const s32 forward = (((s32)cos_a * dx) + ((s32)sin_a * dy)) >> FX_SHIFT;
    const s32 side = (((s32)cos_a * dy) - ((s32)sin_a * dx)) >> FX_SHIFT;
    if ((forward <= BILLBOARD_MIN_DEPTH) || (forward >= type->max_depth)) return FALSE;
    BillboardGeometry geometry;
    billboard_get_geometry(object, type, &geometry);
    measure->type = type;
    measure->forward = forward;
    measure->side = side;
    measure->center_col = (s16)(RAY_VIEW_CENTER_X + bb_divs(side * RAY_PROJ_X, forward));
    measure->atlas_x = geometry.atlas_x;
    measure->atlas_y = geometry.atlas_y;
    measure->atlas_w = geometry.atlas_w;
    measure->atlas_h = geometry.atlas_h;

    if (geometry.uses_wad_origin) {
        // Two native DIVU.W operations per object produce the Q12 screen scale.
        // All patch edges then use MULS.W + shifts, avoiding any extra divisions.
        const u16 scale_x_q12 = divu((u32)RAY_PROJ_X << BILLBOARD_SCALE_SHIFT,
                                     (u16)forward);
        const u16 scale_y_q12 = divu((u32)RAY_PROJ_Y << BILLBOARD_SCALE_SHIFT,
                                     (u16)forward);
        const s16 origin_y = (s16)(RAY_VIEW_CENTER_Y +
            billboard_project_q12(RAY_CAMERA_HEIGHT, scale_y_q12));
        const s16 right_extent = billboard_project_world_q12(
            (s16)(geometry.source_w - geometry.left_offset), scale_x_q12);
        // Sprite pixels must use one focal scale on both axes or the 23x32 Doom
        // barrel is crushed into a bucket by RAY_PROJ_Y/RAY_PROJ_X (120/180).
        // The wall-camera scale remains correct only for locating the floor origin.
        const s16 bottom_extent = billboard_project_world_q12(
            (s16)(geometry.source_h - geometry.top_offset), scale_x_q12);

        measure->left = (s16)(measure->center_col -
            billboard_project_world_q12(geometry.left_offset, scale_x_q12));
        measure->right = (s16)(measure->center_col + right_extent - 1);
        measure->top = (s16)(origin_y -
            billboard_project_world_q12(geometry.top_offset, scale_x_q12));
        measure->bottom = (s16)(origin_y + bottom_extent - 1);
        if (measure->right < measure->left) measure->right = measure->left;
        if (measure->bottom < measure->top) measure->bottom = measure->top;
        measure->half_w = (s16)(measure->center_col - measure->left);
        if ((measure->right - measure->center_col) > measure->half_w) {
            measure->half_w = (s16)(measure->right - measure->center_col);
        }
        measure->projected_height = (s16)(measure->bottom - measure->top + 1);
    } else {
        measure->half_w = (s16)(divu((u32)BILLBOARD_ENEMY_WORLD_WIDTH * RAY_PROJ_X,
                                     (u16)forward) / 2);
        if (measure->half_w < 1) measure->half_w = 1;
        measure->projected_height = (s16)divu(
            (u32)BILLBOARD_ENEMY_WORLD_HEIGHT * RAY_PROJ_Y, (u16)forward);
        if (measure->projected_height < 1) measure->projected_height = 1;
        measure->left = (s16)(measure->center_col - measure->half_w);
        measure->right = (s16)(measure->center_col + measure->half_w);
        measure->bottom = (s16)(RAY_VIEW_CENTER_Y -
            (((s32)-PLAYER_EYE_HEIGHT * RAY_PROJ_Y) / forward));
        measure->top = (s16)(measure->bottom - measure->projected_height);
    }
    return TRUE;
}

void billboard_init(u16 phase_index) {
    u16 count = 0;
    (void)phase_index;
    billboard_registry_reset();
    for (u16 i = 0; i < BILLBOARD_OBJECT_COUNT; i++) g_billboards[i].active = FALSE;
    for (u16 i = 0; i < bsp_thing_count && count < BILLBOARD_OBJECT_COUNT; i++) {
        const u16 flags = bsp_things[i].flags;
        u8 visual = 0;

        // Runtime policy: Doom medium skill, single-player. The generated map
        // remains source-faithful and keeps every THING for future skill/menu work.
        if ((flags & DOOM_THING_SKILL_MEDIUM) == 0 ||
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

BillboardPickupResult billboard_collect_near(s32 x, s32 y) {
    BillboardPickupResult result = {FALSE, BILLBOARD_EFFECT_NONE, 0, BSP_KEY_NONE,
                                    BILLBOARD_PICKUP_NONE};
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
        else if (type->effect == BILLBOARD_EFFECT_AMMO) result.amount = (object->type_id == BILLBOARD_TYPE_AMMO_BOX) ? 20 : 10;
        else if (type->effect == BILLBOARD_EFFECT_KEY) {
            if (object->hp == BILLBOARD_VISUAL_BLUE_KEY) result.key_mask = BSP_KEY_BLUE;
            else if (object->hp == BILLBOARD_VISUAL_YELLOW_KEY) result.key_mask = BSP_KEY_YELLOW;
            else if (object->hp == BILLBOARD_VISUAL_RED_KEY) result.key_mask = BSP_KEY_RED;
            result.kind = BILLBOARD_PICKUP_KEY;
        } else { result.kind = BILLBOARD_PICKUP_BONUS; g_pickup_counts.bonus++; }
        g_collected_count++;
        g_last_pickup_kind = result.kind;
        return result;
    }
    return result;
}

bool billboard_position_blocked(s32 x, s32 y, s32 radius) {
    const u8 *indices = billboard_registry_active_indices();
    const u16 active_count = billboard_registry_active_count();
    for (u16 slot = 0; slot < active_count; slot++) {
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
BillboardPickupCounts billboard_get_pickup_counts(void) { return g_pickup_counts; }
BillboardPickupKind billboard_get_last_pickup_kind(void) { return g_last_pickup_kind; }
u16 billboard_get_enemy_count(void) { return billboard_registry_living_enemy_count(); }
u16 billboard_get_active_count(void) { return billboard_registry_active_count(); }
#if DEBUG_PERF
u16 billboard_get_debug_prop_collision_candidates(void) { return g_debug_prop_collision_candidates; }
u16 billboard_get_debug_visibility_cache_hits(void) { return g_debug_visibility_cache_hits; }
u16 billboard_get_debug_visibility_cache_misses(void) { return g_debug_visibility_cache_misses; }
void billboard_debug_reset_stats(void) {
    g_debug_prop_collision_candidates = 0;
    g_debug_visibility_cache_hits = 0;
    g_debug_visibility_cache_misses = 0;
}
#endif
