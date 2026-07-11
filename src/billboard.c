#include "billboard_internal.h"

// type: visual, effect, HP, radius, scale, max depth, collectible, targetable, blocking
static const BillboardType BILLBOARD_TYPES[BILLBOARD_TYPE_COUNT] = {
    {BILLBOARD_VISUAL_BONUS,       BILLBOARD_EFFECT_HEALTH, 1, 20, BILLBOARD_ITEM_SCALE, BILLBOARD_MAX_DEPTH, TRUE,  FALSE, FALSE},
    {BILLBOARD_VISUAL_BLUE_KEY,    BILLBOARD_EFFECT_KEY,    1, 20, BILLBOARD_ITEM_SCALE, BILLBOARD_MAX_DEPTH, TRUE,  FALSE, FALSE},
    {BILLBOARD_VISUAL_STIMPACK,    BILLBOARD_EFFECT_HEALTH, 1, 20, BILLBOARD_ITEM_SCALE, BILLBOARD_MAX_DEPTH, TRUE,  FALSE, FALSE},
    {BILLBOARD_VISUAL_MEDIKIT,     BILLBOARD_EFFECT_HEALTH, 1, 24, BILLBOARD_ITEM_SCALE, BILLBOARD_MAX_DEPTH, TRUE,  FALSE, FALSE},
    {BILLBOARD_VISUAL_GREEN_ARMOR, BILLBOARD_EFFECT_ARMOR,  1, 28, BILLBOARD_ITEM_SCALE, BILLBOARD_MAX_DEPTH, TRUE,  FALSE, FALSE},
    {BILLBOARD_VISUAL_BLUE_ARMOR,  BILLBOARD_EFFECT_ARMOR,  1, 28, BILLBOARD_ITEM_SCALE, BILLBOARD_MAX_DEPTH, TRUE,  FALSE, FALSE},
    {BILLBOARD_VISUAL_CLIP,        BILLBOARD_EFFECT_AMMO,   1, 18, BILLBOARD_ITEM_SCALE, BILLBOARD_MAX_DEPTH, TRUE,  FALSE, FALSE},
    {BILLBOARD_VISUAL_AMMO_BOX,    BILLBOARD_EFFECT_AMMO,   1, 24, BILLBOARD_ITEM_SCALE, BILLBOARD_MAX_DEPTH, TRUE,  FALSE, FALSE},
    {BILLBOARD_VISUAL_CANDLE,      BILLBOARD_EFFECT_NONE,   1, BILLBOARD_PROP_RADIUS, BILLBOARD_PROP_SCALE, BILLBOARD_MAX_DEPTH, FALSE, FALSE, TRUE},
    {BILLBOARD_VISUAL_CANDELABRA,  BILLBOARD_EFFECT_NONE,   1, BILLBOARD_PROP_RADIUS, BILLBOARD_PROP_SCALE, BILLBOARD_MAX_DEPTH, FALSE, FALSE, TRUE},
    {BILLBOARD_VISUAL_COLUMN,      BILLBOARD_EFFECT_NONE,   1, 48, BILLBOARD_PROP_SCALE, BILLBOARD_MAX_DEPTH, FALSE, FALSE, TRUE},
    {BILLBOARD_VISUAL_BARREL,      BILLBOARD_EFFECT_NONE,   1, BILLBOARD_PROP_RADIUS, BILLBOARD_PROP_SCALE, BILLBOARD_MAX_DEPTH, FALSE, FALSE, TRUE},
    {BILLBOARD_VISUAL_TREE,        BILLBOARD_EFFECT_NONE,   1, 48, BILLBOARD_PROP_SCALE, BILLBOARD_MAX_DEPTH, FALSE, FALSE, TRUE},
    {BILLBOARD_VISUAL_DUMMY,       BILLBOARD_EFFECT_NONE,   3, 24, BILLBOARD_SCALE, BILLBOARD_MAX_DEPTH, FALSE, TRUE,  FALSE},
};

BillboardObject g_billboards[BILLBOARD_OBJECT_COUNT];
u16 g_collected_count;
BillboardPickupCounts g_pickup_counts;
BillboardPickupKind g_last_pickup_kind;
#if DEBUG_PERF
static u16 g_debug_prop_collision_candidates;
#endif

static u8 map_thing_type(u16 doom_type, u8 *visual) {
    switch (doom_type) {
        case 5:  *visual = BILLBOARD_VISUAL_BLUE_KEY; return BILLBOARD_TYPE_KEY;
        case 6:  *visual = BILLBOARD_VISUAL_YELLOW_KEY; return BILLBOARD_TYPE_KEY;
        case 13: *visual = BILLBOARD_VISUAL_RED_KEY; return BILLBOARD_TYPE_KEY;
        case 2014: *visual = BILLBOARD_VISUAL_BONUS; return BILLBOARD_TYPE_BONUS;
        case 2015: *visual = BILLBOARD_VISUAL_GREEN_ARMOR; return BILLBOARD_TYPE_GREEN_ARMOR;
        case 2011: *visual = BILLBOARD_VISUAL_STIMPACK; return BILLBOARD_TYPE_STIMPACK;
        case 2012: *visual = BILLBOARD_VISUAL_MEDIKIT; return BILLBOARD_TYPE_MEDIKIT;
        case 2018: *visual = BILLBOARD_VISUAL_GREEN_ARMOR; return BILLBOARD_TYPE_GREEN_ARMOR;
        case 2019: *visual = BILLBOARD_VISUAL_BLUE_ARMOR; return BILLBOARD_TYPE_BLUE_ARMOR;
        case 2007: *visual = BILLBOARD_VISUAL_CLIP; return BILLBOARD_TYPE_CLIP;
        case 2048: *visual = BILLBOARD_VISUAL_AMMO_BOX; return BILLBOARD_TYPE_AMMO_BOX;
        case 34: *visual = BILLBOARD_VISUAL_CANDLE; return BILLBOARD_TYPE_CANDLE;
        case 35: case 44: case 45: *visual = BILLBOARD_VISUAL_CANDELABRA; return BILLBOARD_TYPE_CANDELABRA;
        // Doom's floor/tall lamps are represented by the tall COLU sprite in
        // the compact atlas; they deliberately block movement like columns.
        case 46: case 47: case 48: case 2028: case 2046: *visual = BILLBOARD_VISUAL_COLUMN; return BILLBOARD_TYPE_COLUMN;
        case 2035: *visual = BILLBOARD_VISUAL_BARREL; return BILLBOARD_TYPE_BARREL;
        case 43: *visual = BILLBOARD_VISUAL_TREE; return BILLBOARD_TYPE_TREE;
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

bool billboard_measure_object(const PlayerState *player, s16 cos_a, s16 sin_a,
                              const BillboardObject *object, BillboardMeasure *measure) {
    if (!object->active) return FALSE;
    const BillboardType *type = billboard_get_type(object->type_id);
    const s32 dx = object->x - player->x;
    const s32 dy = object->y - player->y;
    const s32 forward = (((s32)cos_a * dx) + ((s32)sin_a * dy)) >> FX_SHIFT;
    const s32 side = (((s32)cos_a * dy) - ((s32)sin_a * dx)) >> FX_SHIFT;
    if ((forward <= BILLBOARD_MIN_DEPTH) || (forward >= type->max_depth)) return FALSE;
    measure->type = type;
    measure->forward = forward;
    measure->side = side;
    measure->center_col = (s16)((RAY_VIEW_COLS / 2) + bb_divs(side * 80, forward));
    measure->half_w = (s16)divu((u32)(type->scale * 4), (u16)forward);
    if (measure->half_w > 12) measure->half_w = 12;
    return TRUE;
}

void billboard_init(u16 phase_index) {
    u16 count = 0;
    (void)phase_index;
    for (u16 i = 0; i < BILLBOARD_OBJECT_COUNT; i++) g_billboards[i].active = FALSE;
    for (u16 i = 0; i < bsp_thing_count && count < BILLBOARD_OBJECT_COUNT; i++) {
        u8 visual = 0;
        const u8 type = map_thing_type(bsp_things[i].type, &visual);
        if (type == 0xFF) continue;
        BillboardObject *object = &g_billboards[count++];
        *object = (BillboardObject){0};
        object->x = bsp_things[i].x;
        object->y = bsp_things[i].y;
        object->type_id = type;
        object->hp = visual; // key color is carried as its visual ID.
        object->active = TRUE;
        object->home_x = object->x;
        object->home_y = object->y;
        object->patrol_dir = 1;
        object->patrol_axis_x = (bool)((count & 1) == 0);
        if (type != BILLBOARD_TYPE_KEY) object->hp = billboard_get_type(type)->hit_points;
    }
    g_collected_count = 0;
    g_pickup_counts.bonus = 0;
    g_pickup_counts.key = 0;
    g_last_pickup_kind = BILLBOARD_PICKUP_NONE;
}

BillboardPickupResult billboard_collect_near(s32 x, s32 y) {
    BillboardPickupResult result = {FALSE, BILLBOARD_EFFECT_NONE, 0, BILLBOARD_PICKUP_NONE};
    for (u16 i = 0; i < BILLBOARD_OBJECT_COUNT; i++) {
        BillboardObject *object = &g_billboards[i];
        const BillboardType *type = billboard_get_type(object->type_id);
        if (!object->active || !type->collectible) continue;
        const s32 dx = object->x - x;
        const s32 dy = object->y - y;
        if ((dx * dx) + (dy * dy) > BILLBOARD_COLLECT_RADIUS_SQ) continue;
        object->active = FALSE;
        result.collected = TRUE;
        result.effect = type->effect;
        if (type->effect == BILLBOARD_EFFECT_HEALTH) result.amount = (object->type_id == BILLBOARD_TYPE_MEDIKIT) ? 25 : 10;
        else if (type->effect == BILLBOARD_EFFECT_ARMOR) result.amount = (object->type_id == BILLBOARD_TYPE_BLUE_ARMOR) ? 200 : 100;
        else if (type->effect == BILLBOARD_EFFECT_AMMO) result.amount = (object->type_id == BILLBOARD_TYPE_AMMO_BOX) ? 20 : 10;
        else if (type->effect == BILLBOARD_EFFECT_KEY) {
            result.amount = 1;
            result.kind = BILLBOARD_PICKUP_KEY;
            g_pickup_counts.key++;
        } else { result.kind = BILLBOARD_PICKUP_BONUS; g_pickup_counts.bonus++; }
        g_collected_count++;
        g_last_pickup_kind = result.kind;
        return result;
    }
    return result;
}

bool billboard_position_blocked(s32 x, s32 y, s32 radius) {
    for (u16 i = 0; i < BILLBOARD_OBJECT_COUNT; i++) {
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

bool billboard_consume_key(void) { if (!g_pickup_counts.key) return FALSE; g_pickup_counts.key--; return TRUE; }
u16 billboard_get_collected_count(void) { return g_collected_count; }
BillboardPickupCounts billboard_get_pickup_counts(void) { return g_pickup_counts; }
BillboardPickupKind billboard_get_last_pickup_kind(void) { return g_last_pickup_kind; }
u16 billboard_get_enemy_count(void) { u16 n = 0; for (u16 i = 0; i < BILLBOARD_OBJECT_COUNT; i++) if (g_billboards[i].active && g_billboards[i].type_id == BILLBOARD_TYPE_DUMMY && g_billboards[i].life_state == ENEMY_ALIVE) n++; return n; }
u16 billboard_get_active_count(void) { u16 n = 0; for (u16 i = 0; i < BILLBOARD_OBJECT_COUNT; i++) if (g_billboards[i].active) n++; return n; }
#if DEBUG_PERF
u16 billboard_get_debug_prop_collision_candidates(void) { return g_debug_prop_collision_candidates; }
void billboard_debug_reset_stats(void) { g_debug_prop_collision_candidates = 0; }
#endif
