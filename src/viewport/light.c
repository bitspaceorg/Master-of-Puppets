/*
 * Master of Puppets — Light Management
 * light.c — Multi-light add/remove/update
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "viewport/viewport_internal.h"
#include <string.h>

MopLight *mop_viewport_add_light(MopViewport *vp, const MopLight *desc) {
    if (!vp || !desc) return NULL;

    /* Find a free slot */
    for (uint32_t i = 0; i < MOP_MAX_LIGHTS; i++) {
        if (!vp->lights[i].active) {
            vp->lights[i] = *desc;
            vp->lights[i].active = true;
            if (vp->light_count <= i) {
                vp->light_count = i + 1;
            }
            return &vp->lights[i];
        }
    }
    return NULL; /* all slots full */
}

void mop_viewport_remove_light(MopViewport *vp, MopLight *light) {
    if (!vp || !light) return;
    light->active = false;
}

void mop_light_set_position(MopLight *l, MopVec3 pos) {
    if (!l) return;
    l->position = pos;
}

void mop_light_set_direction(MopLight *l, MopVec3 dir) {
    if (!l) return;
    l->direction = dir;
}

void mop_light_set_color(MopLight *l, MopColor color) {
    if (!l) return;
    l->color = color;
}

void mop_light_set_intensity(MopLight *l, float intensity) {
    if (!l) return;
    l->intensity = intensity;
}

uint32_t mop_viewport_light_count(const MopViewport *vp) {
    if (!vp) return 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < MOP_MAX_LIGHTS; i++) {
        if (vp->lights[i].active) count++;
    }
    return count;
}
