#include <genesis.h>
#include "fixed_math.h"
#include "player_controller.h"
#include "raycast.h"
#include "renderer.h"

#define COUNTER_UPDATE_MASK 0x0F

static PlayerState g_player;
static RayColumn g_ray_columns[RAY_VIEW_COLS];

static void render_current_view(void) {
    raycast_cast_frame(&g_player, g_ray_columns);
    renderer_render_scene(g_ray_columns, g_player.angle);
}

int main(bool hard) {
    u32 frame = 0;
    bool redraw = TRUE;

    (void)hard;

    JOY_init();
    fx_init_tables();
    raycast_init();
    player_init(&g_player);
    renderer_init();
    renderer_draw_static_screen();

    while (TRUE) {
        JOY_update();

        if (player_controller_update(&g_player)) {
            redraw = TRUE;
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
