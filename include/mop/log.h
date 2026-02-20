/*
 * Master of Puppets — Backend-Agnostic Viewport Rendering Engine
 * log.h — Callback-based logging with compile-time level filtering
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_LOG_H
#define MOP_LOG_H

#include <stdarg.h>

/* -------------------------------------------------------------------------
 * Log levels
 * ------------------------------------------------------------------------- */

typedef enum MopLogLevel {
    MOP_LOG_DEBUG = 0,
    MOP_LOG_INFO  = 1,
    MOP_LOG_WARN  = 2,
    MOP_LOG_ERROR = 3
} MopLogLevel;

/* -------------------------------------------------------------------------
 * Log callback
 *
 * Applications can install a custom callback to redirect log output.
 * The default callback writes to stderr.
 * ------------------------------------------------------------------------- */

typedef void (*MopLogCallback)(MopLogLevel level, const char *file,
                               int line, const char *fmt, va_list args);

/* Set a custom log callback.  Pass NULL to restore the default. */
void mop_log_set_callback(MopLogCallback cb);

/* Set the minimum log level.  Messages below this level are discarded. */
void mop_log_set_level(MopLogLevel level);

/* -------------------------------------------------------------------------
 * Internal log function — prefer the macros below
 * ------------------------------------------------------------------------- */

void mop_log_emit(MopLogLevel level, const char *file, int line,
                  const char *fmt, ...);

/* -------------------------------------------------------------------------
 * Logging macros
 *
 * Compile-time filtering: define MOP_LOG_MIN_LEVEL to suppress lower levels.
 *   0 = DEBUG, 1 = INFO, 2 = WARN, 3 = ERROR
 * Default: 0 (all levels enabled)
 * ------------------------------------------------------------------------- */

#ifndef MOP_LOG_MIN_LEVEL
#define MOP_LOG_MIN_LEVEL 0
#endif

#if MOP_LOG_MIN_LEVEL <= 0
#define MOP_DEBUG(...) mop_log_emit(MOP_LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#else
#define MOP_DEBUG(...) ((void)0)
#endif

#if MOP_LOG_MIN_LEVEL <= 1
#define MOP_INFO(...)  mop_log_emit(MOP_LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#else
#define MOP_INFO(...)  ((void)0)
#endif

#if MOP_LOG_MIN_LEVEL <= 2
#define MOP_WARN(...)  mop_log_emit(MOP_LOG_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#else
#define MOP_WARN(...)  ((void)0)
#endif

#if MOP_LOG_MIN_LEVEL <= 3
#define MOP_ERROR(...) mop_log_emit(MOP_LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#else
#define MOP_ERROR(...) ((void)0)
#endif

#endif /* MOP_LOG_H */
