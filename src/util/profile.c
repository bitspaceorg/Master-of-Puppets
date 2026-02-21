/*
 * Master of Puppets — Profiling
 * profile.c — High-resolution timing helpers and stats retrieval
 *
 * Uses mach_absolute_time on macOS, clock_gettime(CLOCK_MONOTONIC) on Linux.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <mop/mop.h>
#include "core/viewport_internal.h"

#ifdef MOP_PLATFORM_MACOS
#include <mach/mach_time.h>

static double mop_time_ms(void) {
    static mach_timebase_info_data_t info;
    if (info.denom == 0) {
        mach_timebase_info(&info);
    }
    uint64_t t = mach_absolute_time();
    /* Convert to milliseconds: t * numer / denom gives nanoseconds */
    return (double)t * (double)info.numer / (double)info.denom / 1e6;
}

#else /* Linux / POSIX */
#include <time.h>

static double mop_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

#endif

double mop_profile_now_ms(void) {
    return mop_time_ms();
}

MopFrameStats mop_viewport_get_stats(const MopViewport *viewport) {
    if (!viewport) {
        return (MopFrameStats){0};
    }
    return viewport->last_stats;
}
