#include "billboard_internal.h"
#include "billboard_explosion.h"
#include "bsp_map.h"

// Last-explosion cache, surfaced to main.c via billboard_get_last_explosion_result()
// when a fire-center call returns BILLBOARD_SHOT_EXPLOSION. Matches the codebase's
// getter-call-after-side-effect pattern (see billboard_get_collected_count()).
static BarrelExplosionResult s_last_explosion;

static void reset_last_explosion(void) {
    s_last_explosion.player_hits = 0;
    s_last_explosion.push_x = 0;
    s_last_explosion.push_y = 0;
}

BarrelExplosionResult billboard_get_last_explosion_result(void) {
    return s_last_explosion;
}

// Worklist entries are world-space blast sites; chain reactions push new sites
// here and the outer loop processes them until empty or the cap is reached.
typedef struct {
    s32 x;
    s32 y;
} BlastSite;

// Deferred actions discovered while walking the active list. We collect them
// first and apply after the loop so billboard_registry_deactivate (which
// mutates s_active_indices[]) never runs mid-iteration.
typedef struct {
    u16 index;
    bool is_barrel;
} PendingHit;

static void process_blast(s32 bx, s32 by, const PlayerState *player,
                          BlastSite *worklist, u16 *worklist_count,
                          BarrelExplosionResult *result) {
    const u8 *indices = billboard_registry_active_indices();
    const u16 active_count = billboard_registry_active_count();
    PendingHit pending[BILLBOARD_OBJECT_COUNT];
    u16 pending_count = 0;

    for (u16 slot = 0; slot < active_count; slot++) {
        const u16 i = indices[slot];
        BillboardObject *object = &g_billboards[i];

        if (!object->active) continue;

        const s32 dx = object->x - bx;
        const s32 dy = object->y - by;
        if ((dx * dx) + (dy * dy) > BARREL_EXPLOSION_RADIUS_SQ) continue;

        // Chain guard: a barrel flagged ENEMY_DYING (i.e. already queued for
        // detonation earlier this call) is short-circuited so each barrel
        // explodes exactly once.
        if (object->life_state != ENEMY_ALIVE) continue;

        if (object->type_id == BILLBOARD_TYPE_BARREL) {
            pending[pending_count].index = i;
            pending[pending_count].is_barrel = TRUE;
            pending_count++;
        } else if (object->type_id == BILLBOARD_TYPE_DUMMY) {
            pending[pending_count].index = i;
            pending[pending_count].is_barrel = FALSE;
            pending_count++;
        }
    }

    for (u16 p = 0; p < pending_count; p++) {
        BillboardObject *object = &g_billboards[pending[p].index];

        if (pending[p].is_barrel) {
            // Detonating barrels keep rendering the BEXP cycle; billboard_update_barrels()
            // drops them from the registry once the last frame has played. The
            // ENEMY_DYING flag is the chain guard so each barrel explodes exactly once.
            object->life_state = ENEMY_DYING;
            object->hp = 0;
            object->death_index = 0;
            object->death_timer = BARREL_DEATH_HOLD;
            if (*worklist_count < BARREL_EXPLOSION_MAX_CHAIN) {
                worklist[*worklist_count].x = object->x;
                worklist[*worklist_count].y = object->y;
                (*worklist_count)++;
            }
        } else {
            // Splash damage: Doom-faithful 20 HP, routed through the same
            // death path the pistol uses for corpse consistency.
            if (object->hp > BARREL_EXPLOSION_DAMAGE) {
                object->hp = (u8)(object->hp - BARREL_EXPLOSION_DAMAGE);
            } else {
                object->hp = 0;
                billboard_registry_enemy_died(pending[p].index);
                object->death_index = 0;
                object->death_timer = ENEMY_DEATH_HOLD;
            }
        }
    }
}

BarrelExplosionResult billboard_apply_explosion(const PlayerState *player,
                                                s32 origin_x, s32 origin_y) {
    reset_last_explosion();
    BlastSite worklist[BARREL_EXPLOSION_MAX_CHAIN];
    u16 worklist_count = 0;

    worklist[worklist_count].x = origin_x;
    worklist[worklist_count].y = origin_y;
    worklist_count++;

    while (worklist_count > 0) {
        worklist_count--;
        const s32 bx = worklist[worklist_count].x;
        const s32 by = worklist[worklist_count].y;

        process_blast(bx, by, player, worklist, &worklist_count, &s_last_explosion);

        // Player AoE: cardinal-normalized knockback from the blast origin, only
        // applied when no wall blocks the segment (parity with pistol LoS).
        const s32 pdx = player->x - bx;
        const s32 pdy = player->y - by;
        if ((pdx * pdx) + (pdy * pdy) <= BARREL_EXPLOSION_RADIUS_SQ) {
            if (!bsp_segment_hits_wall(bx, by, player->x, player->y)) {
                s_last_explosion.player_hits++;
                if (s_last_explosion.push_x == 0 && s_last_explosion.push_y == 0) {
                    s_last_explosion.push_x = (pdx > 0) ? 1 : ((pdx < 0) ? -1 : 0);
                    s_last_explosion.push_y = (pdy > 0) ? 1 : ((pdy < 0) ? -1 : 0);
                }
            }
        }
    }

    return s_last_explosion;
}
