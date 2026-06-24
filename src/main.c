#include <genesis.h>
#include "billboard.h"
#include "fixed_math.h"
#include "player_controller.h"
#include "raycast.h"
#include "renderer.h"
#include "world_map.h"

#define COUNTER_UPDATE_MASK 0x0F
#define SHOT_COOLDOWN_FRAMES 12
#define WEAPON_FLASH_FRAMES 2
#define PLAYER_DAMAGE_FLASH_FRAMES 6
#define PLAYER_INVULN_FRAMES 24
#define PLAYER_HIT_PUSH_STEP (FX_ONE / 4)
#define PLAYER_MAX_HEALTH 3

static PlayerState g_player;
static RayColumn g_ray_columns[RAY_VIEW_COLS];
static u16 g_weapon_flash = 0;
static u16 g_player_damage_flash = 0;
static u16 g_player_invuln = 0;

static void render_current_view(u16 player_health) {
    raycast_cast_frame(&g_player, g_ray_columns);
    renderer_render_scene(
        g_ray_columns, &g_player, g_weapon_flash > 0, g_player_damage_flash > 0, (bool)(player_health <= 1));
}

static void reset_level(u16 phase_index, bool *level_cleared, u16 *shot_cooldown, u16 *player_health, u32 *frame) {
    world_map_init(phase_index);
    billboard_init(phase_index);
    player_init(&g_player, phase_index);
    g_weapon_flash = 0;
    g_player_damage_flash = 0;
    g_player_invuln = 0;
    *level_cleared = FALSE;
    *shot_cooldown = 0;
    *player_health = PLAYER_MAX_HEALTH;
    *frame = 0;

    renderer_draw_static_screen();
    renderer_draw_enemy_count(billboard_get_enemy_count());
    renderer_draw_player_health(*player_health);
    renderer_draw_phase(phase_index);
    renderer_draw_pickup_counter(billboard_get_pickup_counts());
    renderer_draw_last_pickup(billboard_get_last_pickup_kind());
    renderer_draw_action_status(DOOR_ACTION_NONE);
    renderer_draw_goal_status(FALSE);
    renderer_draw_shot_status(BILLBOARD_SHOT_NONE);
    renderer_draw_shot_cooldown(0);
    renderer_draw_target_count(billboard_get_target_count());
    renderer_draw_target_health(billboard_get_target_health());
}

int main(bool hard) {
    u32 frame = 0;
    bool redraw = TRUE;
    bool level_cleared = FALSE;
    u16 phase_index = 0;
    u16 player_health = PLAYER_MAX_HEALTH;
    u16 shot_cooldown = 0;

    (void)hard;

    JOY_init();
    fx_init_tables();
    raycast_init();
    renderer_init();
    reset_level(phase_index, &level_cleared, &shot_cooldown, &player_health, &frame);

    while (TRUE) {
        u16 control = 0;

        if (shot_cooldown > 0) {
            shot_cooldown--;
            renderer_draw_shot_cooldown(shot_cooldown);
        }
        if (g_weapon_flash > 0) {
            g_weapon_flash--;
            redraw = TRUE;
        }
        if (g_player_damage_flash > 0) {
            g_player_damage_flash--;
            redraw = TRUE;
        }
        if (g_player_invuln > 0) {
            g_player_invuln--;
        }

        JOY_update();
        if (!level_cleared) {
            control = player_controller_update(&g_player);
        } else if ((JOY_readJoypad(JOY_1) & BUTTON_START) != 0) {
            phase_index = (u16)((phase_index + 1) & 1);
            reset_level(phase_index, &level_cleared, &shot_cooldown, &player_health, &frame);
            redraw = TRUE;
        }

        if ((control & PLAYER_CONTROL_CHANGED) != 0) {
            redraw = TRUE;
            if (billboard_collect_near(g_player.x, g_player.y)) {
                redraw = TRUE;
                renderer_draw_pickup_counter(billboard_get_pickup_counts());
                renderer_draw_last_pickup(billboard_get_last_pickup_kind());
            }
        }

        if ((control & PLAYER_CONTROL_USE) != 0) {
            const bool has_key = billboard_get_pickup_counts().key > 0;
            const u16 target_count = billboard_get_target_count();
            bool consumed_key = FALSE;
            DoorActionResult action =
                world_map_toggle_door_in_front(g_player.x, g_player.y, g_player.angle, has_key, &consumed_key);

            if ((action == DOOR_ACTION_EXIT) && (target_count > 0)) {
                action = DOOR_ACTION_EXIT_LOCKED;
            }

            renderer_draw_action_status(action);

            if (action != DOOR_ACTION_NONE) {
                if (consumed_key) {
                    billboard_consume_key();
                    renderer_draw_pickup_counter(billboard_get_pickup_counts());
                }
                if (action == DOOR_ACTION_EXIT) {
                    level_cleared = TRUE;
                    renderer_draw_goal_status(TRUE);
                }
                if (action != DOOR_ACTION_EXIT_LOCKED) {
                    redraw = TRUE;
                }
            }
        }

        if ((control & PLAYER_CONTROL_FIRE) != 0) {
            BillboardShotResult shot = BILLBOARD_SHOT_NONE;

            if (shot_cooldown == 0) {
                shot = billboard_fire_center(&g_player, g_ray_columns[RAY_VIEW_COLS / 2].depth);
                shot_cooldown = SHOT_COOLDOWN_FRAMES;
                g_weapon_flash = WEAPON_FLASH_FRAMES;
                renderer_draw_shot_cooldown(shot_cooldown);
                renderer_draw_enemy_count(billboard_get_enemy_count());
                renderer_draw_target_count(billboard_get_target_count());
                renderer_draw_target_health(billboard_get_target_health());
                redraw = TRUE;

                if ((shot == BILLBOARD_SHOT_DAMAGE) || (shot == BILLBOARD_SHOT_KILL)) {
                    redraw = TRUE;
                }
            }

            renderer_draw_shot_status(shot);
        }

        if (!level_cleared) {
            const BillboardEnemyUpdate enemy_update = billboard_update_enemies(&g_player);

            if (enemy_update.moved) {
                redraw = TRUE;
            }

            if ((enemy_update.hits > 0) && (g_player_invuln == 0)) {
                if (player_health <= 1) {
                    reset_level(phase_index, &level_cleared, &shot_cooldown, &player_health, &frame);
                    redraw = TRUE;
                } else {
                    player_apply_world_push(&g_player,
                                            (s32)enemy_update.push_x * PLAYER_HIT_PUSH_STEP,
                                            (s32)enemy_update.push_y * PLAYER_HIT_PUSH_STEP);
                    player_health--;
                    g_player_damage_flash = PLAYER_DAMAGE_FLASH_FRAMES;
                    g_player_invuln = PLAYER_INVULN_FRAMES;
                    renderer_draw_player_health(player_health);
                    redraw = TRUE;
                }
            }
        }

        if (redraw) {
            render_current_view(player_health);
            redraw = FALSE;
        }

        if ((frame & COUNTER_UPDATE_MASK) == 0) {
            renderer_draw_frame_counter(frame);
        }

        VDP_waitVSync();
        frame++;
    }

    return 0;
}
