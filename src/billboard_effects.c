#include "billboard_effects.h"
#include "billboard_internal.h"
#include "fixed_math.h"
#include "generated_billboard_assets.h"

#define IMPACT_POOL_COUNT BILLBOARD_MAX_PROJECTED_EFFECTS
#define IMPACT_FRAME_HOLD 2
#define IMPACT_WORLD_SCALE 1
#define IMPACT_MIN_SCREEN_RADIUS 2
// RAY_PROJ_X<<12 still fits a u16 reciprocal scale at depth 12 and beyond.
#define IMPACT_MIN_DEPTH 11

typedef enum {
    IMPACT_PUFF = 0,
    IMPACT_BLOOD = 1
} ImpactType;

typedef struct {
    s32 x;
    s32 y;
    u8 type;
    u8 frame;
    u8 timer;
    bool active;
} BillboardImpact;

static BillboardImpact s_impacts[IMPACT_POOL_COUNT];
static u8 s_replace_cursor;

static s32 impact_muls_word(s16 left, s16 right) {
    s32 result = left;
    __asm__ volatile (
        "muls.w %1,%0"
        : "+d" (result)
        : "d" (right)
        : "cc");
    return result;
}

static void spawn_impact(s32 x, s32 y, u8 type) {
    u8 slot = s_replace_cursor;

    for (u8 i = 0; i < IMPACT_POOL_COUNT; i++) {
        if (!s_impacts[i].active) {
            slot = i;
            break;
        }
    }

    s_impacts[slot].x = x;
    s_impacts[slot].y = y;
    s_impacts[slot].type = type;
    s_impacts[slot].frame = 0;
    s_impacts[slot].timer = IMPACT_FRAME_HOLD;
    s_impacts[slot].active = TRUE;
    s_replace_cursor = (u8)((slot + 1) % IMPACT_POOL_COUNT);
}

void billboard_effects_reset(void) {
    for (u8 i = 0; i < IMPACT_POOL_COUNT; i++) s_impacts[i].active = FALSE;
    s_replace_cursor = 0;
}

void billboard_effects_spawn_puff(s32 x, s32 y) {
    spawn_impact(x, y, IMPACT_PUFF);
}

void billboard_effects_spawn_blood(s32 x, s32 y) {
    spawn_impact(x, y, IMPACT_BLOOD);
}

bool billboard_update_effects(void) {
    bool changed = FALSE;

    for (u8 i = 0; i < IMPACT_POOL_COUNT; i++) {
        BillboardImpact *impact = &s_impacts[i];
        const u8 frame_count = (impact->type == IMPACT_PUFF) ?
            FREEDOOM_BILLBOARD_PUFF_FRAME_COUNT :
            FREEDOOM_BILLBOARD_BLOOD_FRAME_COUNT;

        if (!impact->active) continue;
        // Hold frames leave the displayed pixels unchanged. Overlay-only work
        // targets the displayed VRAM bank, while a later base redraw completely
        // rebuilds the inactive bank, so only pose transitions and final clear
        // need another render/upload.
        if (impact->timer > 1) {
            impact->timer--;
            continue;
        }
        if ((impact->frame + 1) < frame_count) {
            impact->frame++;
            impact->timer = IMPACT_FRAME_HOLD;
        } else {
            impact->active = FALSE;
        }
        changed = TRUE;
    }

    return changed;
}

static s16 project_scaled(s16 value, u16 scale_q12) {
    const s32 product = impact_muls_word(
        (s16)(value * IMPACT_WORLD_SCALE), (s16)scale_q12);
    return (s16)(product >> 12);
}

u16 billboard_project_effects(const PlayerState *player,
                              ProjectedBillboard *objects,
                              u16 max_objects) {
    const s16 cos_a = fx_cos(player->angle);
    const s16 sin_a = fx_sin(player->angle);
    u16 count = 0;

    for (u8 i = 0; i < IMPACT_POOL_COUNT && count < max_objects; i++) {
        const BillboardImpact *impact = &s_impacts[i];
        if (!impact->active) continue;
        const s32 dx = impact->x - player->x;
        const s32 dy = impact->y - player->y;
        const s32 forward = (impact_muls_word(cos_a, (s16)dx) +
                             impact_muls_word(sin_a, (s16)dy)) >> FX_SHIFT;
        const s32 side = (impact_muls_word(cos_a, (s16)dy) -
                          impact_muls_word(sin_a, (s16)dx)) >> FX_SHIFT;
        const s16 *geometry;
        u16 scale_q12;
        s16 center_col;
        ProjectedBillboard *projected;

        if (forward <= IMPACT_MIN_DEPTH || forward > 0x7FFF) continue;

        if (impact->type == IMPACT_PUFF) {
            geometry = FREEDOOM_BILLBOARD_PUFF_GEOMETRY[impact->frame];
        } else {
            geometry = FREEDOOM_BILLBOARD_BLOOD_GEOMETRY[impact->frame];
        }
        scale_q12 = divu((u32)RAY_PROJ_X << 12, (u16)forward);
        center_col = (s16)(RAY_VIEW_CENTER_X +
            divs(impact_muls_word((s16)side, RAY_PROJ_X), (s16)forward));
        projected = &objects[count];
        projected->left = (s16)(center_col - project_scaled(geometry[2], scale_q12));
        projected->right = (s16)(center_col +
            project_scaled((s16)(geometry[0] - geometry[2]), scale_q12) - 1);
        projected->top = (s16)(RAY_VIEW_CENTER_Y - project_scaled(geometry[3], scale_q12));
        projected->bottom = (s16)(RAY_VIEW_CENTER_Y +
            project_scaled((s16)(geometry[1] - geometry[3]), scale_q12) - 1);
        // A distant native crop can quantize to one screen pixel. Preserve a
        // small readable mark like Doom's puff instead of silently becoming an
        // effectively invisible speck.
        if ((projected->right - projected->left) < (IMPACT_MIN_SCREEN_RADIUS * 2)) {
            projected->left = (s16)(center_col - IMPACT_MIN_SCREEN_RADIUS);
            projected->right = (s16)(center_col + IMPACT_MIN_SCREEN_RADIUS);
        }
        if ((projected->bottom - projected->top) < (IMPACT_MIN_SCREEN_RADIUS * 2)) {
            projected->top = (s16)(RAY_VIEW_CENTER_Y - IMPACT_MIN_SCREEN_RADIUS);
            projected->bottom = (s16)(RAY_VIEW_CENTER_Y + IMPACT_MIN_SCREEN_RADIUS);
        }
        projected->depth = (u16)forward;
        projected->visual_id = (impact->type == IMPACT_PUFF) ?
            BILLBOARD_VISUAL_PUFF : BILLBOARD_VISUAL_BLOOD;
        projected->frame = impact->frame;
        projected->atlas_x = 0;
        projected->atlas_y = 0;
        projected->atlas_w = (u8)geometry[0];
        projected->atlas_h = (u8)geometry[1];
        count++;
    }

    return count;
}
