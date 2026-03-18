/*
 * Master of Puppets — Subsystem Registry
 * subsystem.c — Registration and phase-based dispatch
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "core/subsystem.h"
#include "core/viewport_internal.h"
#include <mop/util/log.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

bool mop_subsystem_register(MopSubsystemRegistry *reg, MopSubsystem *sub) {
  if (!reg || !sub || !sub->vtable)
    return false;

  if (reg->count >= reg->capacity) {
    uint32_t new_cap =
        reg->capacity ? reg->capacity * 2 : MOP_INITIAL_SUBSYSTEM_CAPACITY;
    MopSubsystem **new_entries =
        realloc(reg->entries, (size_t)new_cap * sizeof(MopSubsystem *));
    if (!new_entries) {
      MOP_WARN("subsystem registry grow failed, cannot register '%s'",
               sub->vtable->name ? sub->vtable->name : "?");
      return false;
    }
    memset(new_entries + reg->capacity, 0,
           (size_t)(new_cap - reg->capacity) * sizeof(MopSubsystem *));
    reg->entries = new_entries;
    reg->capacity = new_cap;
  }

  reg->entries[reg->count++] = sub;
  return true;
}

bool mop_subsystem_unregister(MopSubsystemRegistry *reg, MopSubsystem *sub) {
  if (!reg || !sub)
    return false;

  for (uint32_t i = 0; i < reg->count; i++) {
    if (reg->entries[i] == sub) {
      /* Shift remaining entries down */
      for (uint32_t j = i; j + 1 < reg->count; j++)
        reg->entries[j] = reg->entries[j + 1];
      reg->entries[--reg->count] = NULL;
      return true;
    }
  }
  return false;
}

void mop_subsystem_dispatch(MopSubsystemRegistry *reg, MopSubsystemPhase phase,
                            MopViewport *vp, float dt, float t) {
  if (!reg)
    return;

  for (uint32_t i = 0; i < reg->count; i++) {
    MopSubsystem *sub = reg->entries[i];
    if (sub && sub->enabled && sub->vtable && sub->vtable->phase == phase &&
        sub->vtable->update) {
      sub->vtable->update(sub, vp, dt, t);
    }
  }
}

void mop_subsystem_destroy_all(MopSubsystemRegistry *reg, MopViewport *vp) {
  if (!reg)
    return;

  for (uint32_t i = 0; i < reg->count; i++) {
    MopSubsystem *sub = reg->entries[i];
    if (sub && sub->vtable && sub->vtable->destroy) {
      sub->vtable->destroy(sub, vp);
    }
    reg->entries[i] = NULL;
  }
  reg->count = 0;
  free(reg->entries);
  reg->entries = NULL;
  reg->capacity = 0;
}
