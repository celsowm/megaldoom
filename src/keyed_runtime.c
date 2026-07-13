#include "billboard_internal.h"
#include "bsp_map.h"
#include "fixed_math.h"

#define KEY_MASK_BLUE 0x01u
#define KEY_MASK_YELLOW 0x02u
#define KEY_MASK_RED 0x04u

static u8 g_key_mask;

static u8 visual_key_mask(u8 visual_id) {
    switch (visual_id) {
        case BILLBOARD_VISUAL_BLUE_KEY: return KEY_MASK_BLUE;
        case BILLBOARD_VISUAL_YELLOW_KEY: return KEY_MASK_YELLOW;
        case BILLBOARD_VISUAL_RED_KEY: return KEY_MASK_RED;
        default: return 0;
    }
}

static u8 special_key_mask(u16 special) {
    switch (special) {
        case 26:
        case 32:
            return KEY_MASK_BLUE;
        case 27:
        case 34:
            return KEY_MASK_YELLOW;
        case 28:
        case 33:
            return KEY_MASK_RED;
        default:
            return 0;
    }
}

static s32 cross3(s32 ax, s32 ay, s32 bx, s32 by, s32 cx, s32 cy) {
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

static bool point_in_line_bounds(const BspVertex *a, const BspVertex *b,
                                 const BspVertex *point) {
    const s16 min_x = a->x < b->x ? a->x : b->x;
    const s16 max_x = a->x > b->x ? a->x : b->x;
    const s16 min_y = a->y < b->y ? a->y : b->y;
    const s16 max_y = a->y > b->y ? a->y : b->y;
    return (bool)(point->x >= min_x && point->x <= max_x &&
                  point->y >= min_y && point->y <= max_y);
}

static bool seg_belongs_to_line(const BspSeg *seg, const BspLine *line) {
    const BspVertex *line_a = &bsp_vertices[line->v1];
    const BspVertex *line_b = &bsp_vertices[line->v2];
    const BspVertex *seg_a = &bsp_vertices[seg->v1];
    const BspVertex *seg_b = &bsp_vertices[seg->v2];
    return (bool)(
        cross3(line_a->x, line_a->y, line_b->x, line_b->y,
               seg_a->x, seg_a->y) == 0 &&
        cross3(line_a->x, line_a->y, line_b->x, line_b->y,
               seg_b->x, seg_b->y) == 0 &&
        point_in_line_bounds(line_a, line_b, seg_a) &&
        point_in_line_bounds(line_a, line_b, seg_b));
}

static u8 locked_seg_key_mask(const BspSeg *seg) {
    for (u16 i = 0; i < bsp_line_count; i++) {
        const BspLine *line = &bsp_lines[i];
        const u8 mask = special_key_mask(line->special);
        if (mask != 0 && seg_belongs_to_line(seg, line)) return mask;
    }
    return 0;
}

static s32 seg_point_dist2(const BspSeg *seg, s32 px, s32 py) {
    const BspVertex *a = &bsp_vertices[seg->v1];
    const BspVertex *b = &bsp_vertices[seg->v2];
    const s32 abx = (s32)b->x - a->x;
    const s32 aby = (s32)b->y - a->y;
    const s32 apx = px - a->x;
    const s32 apy = py - a->y;
    const s32 ab2 = abx * abx + aby * aby;
    s32 closest_x;
    s32 closest_y;

    if (ab2 <= 0) {
        closest_x = a->x;
        closest_y = a->y;
    } else {
        const s32 dot = apx * abx + apy * aby;
        if (dot <= 0) {
            closest_x = a->x;
            closest_y = a->y;
        } else if (dot >= ab2) {
            closest_x = b->x;
            closest_y = b->y;
        } else {
            const s32 fraction = (dot << FX_SHIFT) / ab2;
            closest_x = a->x + ((abx * fraction) >> FX_SHIFT);
            closest_y = a->y + ((aby * fraction) >> FX_SHIFT);
        }
    }

    {
        const s32 dx = px - closest_x;
        const s32 dy = py - closest_y;
        return dx * dx + dy * dy;
    }
}

void keyed_billboard_init(u16 phase_index) {
    g_key_mask = 0;
    billboard_init(phase_index);
}

BillboardPickupResult keyed_billboard_collect_near(s32 x, s32 y) {
    u8 pending_key_mask = 0;

    /* Mirror billboard_collect_near's first-match policy before it deactivates
       the object. This preserves the key colour without expanding HUD structs. */
    for (u16 i = 0; i < BILLBOARD_OBJECT_COUNT; i++) {
        const BillboardObject *object = &g_billboards[i];
        const BillboardType *type = billboard_get_type(object->type_id);
        const s32 dx = object->x - x;
        const s32 dy = object->y - y;
        if (!object->active || !type->collectible) continue;
        if (dx * dx + dy * dy > BILLBOARD_COLLECT_RADIUS_SQ) continue;
        if (type->effect == BILLBOARD_EFFECT_KEY) {
            pending_key_mask = visual_key_mask(object->hp);
        }
        break;
    }

    {
        const BillboardPickupResult result = billboard_collect_near(x, y);
        if (result.collected && result.effect == BILLBOARD_EFFECT_KEY) {
            g_key_mask |= pending_key_mask;
        }
        return result;
    }
}

DoorActionResult keyed_bsp_use_in_front(s32 x, s32 y, u16 angle,
                                        bool has_any_key,
                                        bool *consumed_key) {
    const s16 direction_x = fx_cos(angle);
    const s16 direction_y = fx_sin(angle);
    (void)has_any_key;

    if (consumed_key != NULL) *consumed_key = FALSE;

    /* Mirror bsp_use_in_front's target selection, but reject a locked segment
       unless the matching reusable Doom key colour has been collected. */
    for (s32 distance = FX_ONE / 2; distance <= FX_ONE * 2;
         distance += FX_ONE / 2) {
        const s32 point_x = x + (((s32)direction_x * distance) >> FX_SHIFT);
        const s32 point_y = y + (((s32)direction_y * distance) >> FX_SHIFT);
        for (u16 i = 0; i < bsp_seg_count; i++) {
            const BspSeg *seg = &bsp_segs[i];
            bool ignored_consumption = FALSE;
            u8 required;

            if (seg->type == BSP_SEG_WALL) continue;
            if (seg_point_dist2(seg, point_x, point_y) >=
                (s32)FX_ONE * FX_ONE) continue;
            if (seg->type != BSP_SEG_LOCKED_DOOR) {
                return bsp_use_in_front(x, y, angle, TRUE, &ignored_consumption);
            }

            required = locked_seg_key_mask(seg);
            if (required == 0) {
                if (g_key_mask == 0) return DOOR_ACTION_LOCKED;
            } else if ((g_key_mask & required) != required) {
                return DOOR_ACTION_LOCKED;
            }
            return bsp_use_in_front(x, y, angle, TRUE, &ignored_consumption);
        }
    }

    return DOOR_ACTION_NONE;
}
