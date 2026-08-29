/* BSP view-space projection and fixed-point arithmetic. */
#include "bsp_render_internal.h"

s32 bsp_native_muls_word(s16 left, s16 right) {
    s32 result = left;
    __asm__ volatile (
        "muls.w %1,%0"
        : "+d" (result)
        : "d" (right)
        : "cc");
    return result;
}

s32 bsp_render_mul(s32 left, s32 right) {
    return bsp_native_muls_word((s16)left, (s16)right);
}

u32 bsp_native_mulu_word(u16 left, u16 right) {
    u32 result = left;
    __asm__ volatile (
        "mulu.w %1,%0"
        : "+d" (result)
        : "d" (right)
        : "cc");
    return result;
}

s32 bsp_native_mul_long_unsigned(s32 left, u16 right) {
    const bool negative = (bool)(left < 0);
    const u32 magnitude = negative ? (u32)(-left) : (u32)left;
    const u32 low = bsp_native_mulu_word((u16)magnitude, right);
    const u32 high = bsp_native_mulu_word((u16)(magnitude >> 16), right) << 16;
    const u32 product = low + high;
    return negative ? -(s32)product : (s32)product;
}

u16 bsp_reciprocal_depth(s32 depth) {
    // Callers clip depth to >= BSP_NEAR (16) first, and the shared inverse
    // table is exact truncating BSP_INV_SCALE/(i+1) (see bsp_inv_depth_lut.h),
    // so table hits are byte-identical to the DIVU they replace. Walls farther
    // than 1024 units keep the native divide.
    if (depth <= 1024) {
        return g_bsp_inv_depth_lut[depth - 1];
    }
    return divu(BSP_INV_SCALE, (u16)depth);
}

s32 bsp_reciprocal_span(s32 span) {
    const u32 numerator = (u32)FX_ONE << FX_SHIFT;

    if (span == 1) {
        return (s32)numerator;
    }
    return (s32)divu(numerator, (u16)span);
}

s32 bsp_perspective_divide(s32 numerator, s32 denominator) {
    return (s32)divs(numerator, (s16)denominator);
}

void bsp_transform_vertex(u16 vertex_index, s32 *depth, s32 *lateral) {
    if (vertex_index < BSP_MAX_VERTICES &&
        g_vertex_generation[vertex_index] == g_cast_generation) {
        *depth = g_vertex_depth[vertex_index];
        *lateral = g_vertex_lateral[vertex_index];
        return;
    }

    const BspVertex *vertex = &bsp_vertices[vertex_index];
    const s32 relx = (s32)vertex->x - g_px;
    const s32 rely = (s32)vertex->y - g_py;
    const s32 transformed_depth =
        (bsp_render_mul(relx, g_fwx) + bsp_render_mul(rely, g_fwy)) >> FX_SHIFT;
    const s32 transformed_lateral =
        (bsp_render_mul(relx, g_rx) + bsp_render_mul(rely, g_ry)) >> FX_SHIFT;

    *depth = transformed_depth;
    *lateral = transformed_lateral;
    if (vertex_index < BSP_MAX_VERTICES) {
        g_vertex_depth[vertex_index] = (s16)transformed_depth;
        g_vertex_lateral[vertex_index] = (s16)transformed_lateral;
        g_vertex_generation[vertex_index] = g_cast_generation;
    }
}

