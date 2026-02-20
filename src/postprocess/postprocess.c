/*
 * Master of Puppets — Post-Processing
 * postprocess.c — Per-pixel post-processing effects on the framebuffer
 *
 * Operates directly on the MopSwFramebuffer color buffer after frame_end.
 * Effects applied in order: fog -> tonemap -> gamma -> vignette.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "viewport/viewport_internal.h"
#include "rasterizer/rasterizer.h"

#include <math.h>

/* -------------------------------------------------------------------------
 * Post-processing application
 *
 * Iterates all pixels in the framebuffer and applies the requested effects.
 * Order: fog -> tonemap -> gamma -> vignette
 * ------------------------------------------------------------------------- */

void mop_postprocess_apply(MopSwFramebuffer *fb, uint32_t effects,
                            const MopFogParams *fog) {
    if (!fb || !fb->color || effects == 0) return;

    int w = fb->width;
    int h = fb->height;
    float cx = (float)w * 0.5f;
    float cy = (float)h * 0.5f;
    float max_dist = sqrtf(cx * cx + cy * cy);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            size_t pi = ((size_t)y * (size_t)w + (size_t)x) * 4;

            float r = (float)fb->color[pi + 0] / 255.0f;
            float g = (float)fb->color[pi + 1] / 255.0f;
            float b = (float)fb->color[pi + 2] / 255.0f;
            /* alpha preserved as-is */

            /* --- Fog --- */
            if ((effects & MOP_POST_FOG) && fog) {
                float depth = fb->depth[(size_t)y * (size_t)w + (size_t)x];
                float range = fog->far_dist - fog->near_dist;
                float factor = 0.0f;
                if (range > 0.0f) {
                    factor = (depth - fog->near_dist) / range;
                    if (factor < 0.0f) factor = 0.0f;
                    if (factor > 1.0f) factor = 1.0f;
                }
                r = r + (fog->color.r - r) * factor;
                g = g + (fog->color.g - g) * factor;
                b = b + (fog->color.b - b) * factor;
            }

            /* --- Tonemap (Reinhard) --- */
            if (effects & MOP_POST_TONEMAP) {
                r = r / (r + 1.0f);
                g = g / (g + 1.0f);
                b = b / (b + 1.0f);
            }

            /* --- Gamma (sRGB transfer function, matches Vulkan R8G8B8A8_SRGB) --- */
            if (effects & MOP_POST_GAMMA) {
                #define LINEAR_TO_SRGB(c) \
                    ((c) <= 0.0031308f \
                        ? (c) * 12.92f \
                        : 1.055f * powf((c), 1.0f / 2.4f) - 0.055f)
                r = LINEAR_TO_SRGB(r > 0.0f ? r : 0.0f);
                g = LINEAR_TO_SRGB(g > 0.0f ? g : 0.0f);
                b = LINEAR_TO_SRGB(b > 0.0f ? b : 0.0f);
                #undef LINEAR_TO_SRGB
            }

            /* --- Vignette --- */
            if (effects & MOP_POST_VIGNETTE) {
                float dx = (float)x - cx;
                float dy = (float)y - cy;
                float dist = sqrtf(dx * dx + dy * dy) / max_dist;
                float vignette = 1.0f - dist * dist * 0.5f;
                if (vignette < 0.0f) vignette = 0.0f;
                r *= vignette;
                g *= vignette;
                b *= vignette;
            }

            /* Clamp and write back */
            if (r > 1.0f) r = 1.0f;
            if (g > 1.0f) g = 1.0f;
            if (b > 1.0f) b = 1.0f;
            if (r < 0.0f) r = 0.0f;
            if (g < 0.0f) g = 0.0f;
            if (b < 0.0f) b = 0.0f;

            fb->color[pi + 0] = (uint8_t)(r * 255.0f + 0.5f);
            fb->color[pi + 1] = (uint8_t)(g * 255.0f + 0.5f);
            fb->color[pi + 2] = (uint8_t)(b * 255.0f + 0.5f);
        }
    }
}

/* -------------------------------------------------------------------------
 * Viewport API for post-processing
 * ------------------------------------------------------------------------- */

void mop_viewport_set_post_effects(MopViewport *viewport, uint32_t effects) {
    if (!viewport) return;
    viewport->post_effects = effects;
}

void mop_viewport_set_fog(MopViewport *viewport, const MopFogParams *fog) {
    if (!viewport || !fog) return;
    viewport->fog_params = *fog;
}
