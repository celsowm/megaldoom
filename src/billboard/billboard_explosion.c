#include "billboard_internal.h"
#include "billboard_explosion.h"
#include "bsp_map.h"

typedef struct {
    s32 x;
    s32 y;
} BlastSite;

typedef struct {
    u16 index;
    u16 damage;
    bool is_barrel;
} PendingHit;

static s32 abs_s32(s32 value) {
    return (value < 0) ? -value : value;
}

u16 billboard_explosion_damage(s32 blast_x, s32 blast_y,
                               s32 target_x, s32 target_y,
                               u16 target_radius) {
    const s32 dx = abs_s32(target_x - blast_x);
    const s32 dy = abs_s32(target_y - blast_y);
    s32 distance = ((dx > dy) ? dx : dy) - target_radius;

    if (distance < 0) distance = 0;
    if (distance >= BARREL_EXPLOSION_RADIUS) return 0;
    // 192 units map linearly onto Doom's 128 damage steps: distance 0 is 128,
    // distance 191 is 1, and the radius boundary is zero.
    return (u16)(BARREL_EXPLOSION_DAMAGE - (((u32)distance * 2u) / 3u));
}

static void process_blast(s32 bx, s32 by,
                          BlastSite *worklist, u16 *worklist_count) {
    // The target registry is exactly the union affected by blast damage:
    // barrels plus enemies. Avoid walking pickups and decorative billboards;
    // moving enemies remain present without requiring a static spatial grid.
    const u8 *indices = billboard_registry_target_indices();
    const u16 target_count = billboard_registry_target_count();
    PendingHit pending[BILLBOARD_OBJECT_COUNT];
    u16 pending_count = 0;

    for (u16 slot = 0; slot < target_count; slot++) {
        const u16 i = indices[slot];
        BillboardObject *object = &g_billboards[i];
        const BillboardType *type;
        u16 damage;

        if (!object->active || object->life_state != ENEMY_ALIVE) continue;
        if (object->type_id != BILLBOARD_TYPE_BARREL &&
            object->type_id != BILLBOARD_TYPE_DUMMY) continue;

        type = billboard_get_type(object->type_id);
        damage = billboard_explosion_damage(bx, by, object->x, object->y, type->radius);
        if (damage == 0) continue;
        if (bsp_segment_crosses_wall(bx, by, object->x, object->y)) continue;

        pending[pending_count].index = i;
        pending[pending_count].damage = damage;
        pending[pending_count].is_barrel = (object->type_id == BILLBOARD_TYPE_BARREL);
        pending_count++;
    }

    for (u16 p = 0; p < pending_count; p++) {
        BillboardObject *object = &g_billboards[pending[p].index];

        if (pending[p].damage < object->hp) {
            object->hp = (u8)(object->hp - pending[p].damage);
            continue;
        }

        object->hp = 0;
        object->death_index = 0;
        if (pending[p].is_barrel) {
            object->life_state = ENEMY_DYING;
            object->death_timer = (u8)(BARREL_DEATH_FRAME_HOLDS[0] + 1);
            if (*worklist_count < BARREL_EXPLOSION_MAX_CHAIN) {
                worklist[*worklist_count].x = object->x;
                worklist[*worklist_count].y = object->y;
                (*worklist_count)++;
            }
        } else {
            billboard_registry_enemy_died(pending[p].index);
            object->death_timer = ENEMY_DEATH_HOLD;
        }
    }
}

BarrelExplosionResult billboard_apply_explosion(const PlayerState *player,
                                                s32 origin_x, s32 origin_y) {
    BarrelExplosionResult result = {0, 0, 0, 0};
    BlastSite worklist[BARREL_EXPLOSION_MAX_CHAIN];
    u16 worklist_count = 1;
    u16 strongest_damage = 0;

    worklist[0].x = origin_x;
    worklist[0].y = origin_y;

    while (worklist_count > 0) {
        u16 player_damage;
        worklist_count--;
        const s32 bx = worklist[worklist_count].x;
        const s32 by = worklist[worklist_count].y;

        if (result.explosion_count < 0xFF) result.explosion_count++;
        process_blast(bx, by, worklist, &worklist_count);

        player_damage = billboard_explosion_damage(
            bx, by, player->x, player->y, PLAYER_COLLISION_RADIUS);
        if (player_damage == 0 ||
            bsp_segment_crosses_wall(bx, by, player->x, player->y)) {
            continue;
        }

        if ((u32)result.player_damage + player_damage > 0xFFFFu) {
            result.player_damage = 0xFFFFu;
        } else {
            result.player_damage = (u16)(result.player_damage + player_damage);
        }
        if (player_damage > strongest_damage) {
            const s32 pdx = player->x - bx;
            const s32 pdy = player->y - by;
            strongest_damage = player_damage;
            result.push_x = (pdx > 0) ? 1 : ((pdx < 0) ? -1 : 0);
            result.push_y = (pdy > 0) ? 1 : ((pdy < 0) ? -1 : 0);
        }
    }

    return result;
}
