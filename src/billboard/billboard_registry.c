#include "billboard_internal.h"

static u8 s_active_indices[BILLBOARD_OBJECT_COUNT];
static u8 s_enemy_indices[BILLBOARD_OBJECT_COUNT];
static u8 s_target_indices[BILLBOARD_OBJECT_COUNT];
static u8 s_blocking_indices[BILLBOARD_OBJECT_COUNT];
static u16 s_active_count;
static u16 s_enemy_count;
static u16 s_target_count;
static u16 s_blocking_count;
static u16 s_living_enemy_count;

static void remove_index_stable(u8 *indices, u16 *count, u16 index) {
    for (u16 i = 0; i < *count; i++) {
        if (indices[i] != index) continue;

        (*count)--;
        for (; i < *count; i++) {
            indices[i] = indices[i + 1];
        }
        return;
    }
}

void billboard_registry_reset(void) {
    s_active_count = 0;
    s_enemy_count = 0;
    s_target_count = 0;
    s_blocking_count = 0;
    s_living_enemy_count = 0;
}

void billboard_registry_add(u16 index) {
    const BillboardType *type = billboard_get_type(g_billboards[index].type_id);

    s_active_indices[s_active_count++] = (u8)index;
    if (g_billboards[index].type_id == BILLBOARD_TYPE_DUMMY) {
        s_enemy_indices[s_enemy_count++] = (u8)index;
        s_living_enemy_count++;
    }
    // s_target_indices is the union of everything pistol fire-center can hit.
    // DUMMY already lands here via the AI branch above; barrels (and any future
    // targetable prop) join the same list so fire_center walks both in one pass.
    if (type->targetable) {
        s_target_indices[s_target_count++] = (u8)index;
    }
    if (type->blocking) {
        s_blocking_indices[s_blocking_count++] = (u8)index;
    }
}

void billboard_registry_deactivate(u16 index) {
    remove_index_stable(s_active_indices, &s_active_count, index);
    remove_index_stable(s_target_indices, &s_target_count, index);
    remove_index_stable(s_blocking_indices, &s_blocking_count, index);
    g_billboards[index].active = FALSE;
}

void billboard_registry_enemy_died(u16 index) {
    BillboardObject *object = &g_billboards[index];
    if ((object->type_id == BILLBOARD_TYPE_DUMMY) &&
        (object->life_state == ENEMY_ALIVE) && (s_living_enemy_count > 0)) {
        s_living_enemy_count--;
    }
    object->life_state = ENEMY_DYING;
}

const u8 *billboard_registry_active_indices(void) { return s_active_indices; }
const u8 *billboard_registry_enemy_indices(void) { return s_enemy_indices; }
const u8 *billboard_registry_target_indices(void) { return s_target_indices; }
const u8 *billboard_registry_blocking_indices(void) { return s_blocking_indices; }
u16 billboard_registry_active_count(void) { return s_active_count; }
u16 billboard_registry_enemy_count(void) { return s_enemy_count; }
u16 billboard_registry_target_count(void) { return s_target_count; }
u16 billboard_registry_blocking_count(void) { return s_blocking_count; }
u16 billboard_registry_living_enemy_count(void) { return s_living_enemy_count; }
