#include <genesis.h>
#include "billboard.h"
#include "fixed_math.h"
#include "player_controller.h"
#include "raycast.h"
#include "renderer.h"
#include "world_map.h"

#define COUNTER_UPDATE_MASK 0x0F

static PlayerState g_player;
static RayColumn g_ray_columns[RAY_VIEW_COLS];

static void render_current_view(void) {
    raycast_cast_frame(&g_player, g_ray_columns);
    renderer_render_scene(g_ray_columns, &g_player);
}

int main(bool hard) {
    u32 frame = 0;
    bool redraw = TRUE;

    (void)hard;

    JOY_init();
    fx_init_tables();
    world_map_init();
    billboard_init();
    raycast_init();
    player_init(&g_player);
    renderer_init();
    renderer_draw_static_screen();
    renderer_draw_pickup_counter(billboard_get_collected_count());

    while (TRUE) {
        JOY_update();
        const u16 control = player_controller_update(&g_player);

        if ((control & PLAYER_CONTROL_CHANGED) != 0) {
            redraw = TRUE;
            if (billboard_collect_near(g_player.x, g_player.y)) {
                redraw = TRUE;
                renderer_draw_pickup_counter(billboard_get_collected_count());
            }
        }

        if ((control & PLAYER_CONTROL_USE) != 0) {
            if (world_map_toggle_door_in_front(g_player.x, g_player.y, g_player.angle)) {
                redraw = TRUE;
            }
        }

        if (redraw) {
            render_current_view();
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
