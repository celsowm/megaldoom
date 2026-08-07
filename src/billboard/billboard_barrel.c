#include "billboard_internal.h"

// Step the barrel explosion animation one frame. A barrel uses the exact
// per-pose cadence and leaves the registry after its final frame, which also
// releases collision and targeting ownership.
static void advance_barrel_death(u16 index, BillboardObject *object) {
    if (object->life_state != ENEMY_DYING) {
        return;
    }
    if (object->death_timer > 1) {
        object->death_timer--;
        return;
    }
    if ((object->death_index + 1) < BARREL_DEATH_FRAME_COUNT) {
        object->death_index++;
        object->death_timer = BARREL_DEATH_FRAME_HOLDS[object->death_index];
    } else {
        billboard_registry_deactivate(index);
    }
}

BillboardEnemyUpdate billboard_update_barrels(const PlayerState *player) {
    BillboardEnemyUpdate update = {FALSE, FALSE, FALSE, 0, 0, 0};
    (void)player;

    // Deactivation mutates the target registry, so iterate a stable snapshot.
    u8 snapshot[BILLBOARD_OBJECT_COUNT];
    const u8 *target_indices = billboard_registry_target_indices();
    const u16 target_count = billboard_registry_target_count();
    for (u16 i = 0; i < target_count; i++) snapshot[i] = target_indices[i];

    for (u16 slot = 0; slot < target_count; slot++) {
        const u16 i = snapshot[slot];
        BillboardObject *object = &g_billboards[i];

        if (!object->active || (object->type_id != BILLBOARD_TYPE_BARREL) ||
            (object->life_state != ENEMY_DYING)) {
            continue;
        }

        const u8 prev_frame = billboard_get_object_frame(object);
        const bool was_active = object->active;
        advance_barrel_death(i, object);
        // Deactivation does not change death_index, so it is a visual transition
        // independently of the ordinary frame comparison.
        if (!object->active && was_active) {
            update.moved = TRUE;
            update.pose_changed = TRUE;
        }
        if (billboard_get_object_frame(object) != prev_frame) {
            update.moved = TRUE;
            update.pose_changed = TRUE;
        }
    }

    return update;
}
