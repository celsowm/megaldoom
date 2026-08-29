/* BSP sample occlusion and coverage queries. */
#include "bsp_render_internal.h"

bool bsp_view_fully_closed(void) {
    return (bool)(g_solid_count >= BSP_SAMPLE_COLS);
}

u16 bsp_find_next_open(u16 sample) {
    u16 root = sample;

    while (g_next_open[root] != root) {
        root = g_next_open[root];
    }
    while (sample != root) {
        const u16 next = g_next_open[sample];
        g_next_open[sample] = (u8)root;
        sample = next;
    }
    return root;
}

void bsp_mark_sample_solid(u16 sample) {
    g_solid_words[sample >> 5] |= (u32)1u << (sample & 31);
    g_solid_count++;
    g_next_open[sample] = (u8)bsp_find_next_open((u16)(sample + 1));
}

bool bsp_solid_sample_range_filled(u16 left_sample, u16 right_sample) {
    u16 word = (u16)(left_sample >> 5);
    const u16 last_word = (u16)(right_sample >> 5);

    while (word <= last_word) {
        const u16 word_left = (u16)(word << 5);
        const u16 word_right = (u16)(word_left + 31);
        const u16 range_left = (left_sample > word_left) ? left_sample : word_left;
        const u16 range_right = (right_sample < word_right) ? right_sample : word_right;
        const u16 start_bit = (u16)(range_left & 31);
        const u16 end_bit = (u16)(range_right & 31);
        u32 mask = 0xFFFFFFFFu << start_bit;

        if (end_bit < 31) {
            mask &= (u32)((1u << (end_bit + 1)) - 1u);
        }
        if ((g_solid_words[word] & mask) != mask) {
            return FALSE;
        }
        word++;
    }

    return TRUE;
}

