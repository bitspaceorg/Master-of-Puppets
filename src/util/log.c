/*
 * Master of Puppets — Logging
 * log.c — Default stderr sink, callback dispatch, level filtering
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <mop/log.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * Static state
 * ------------------------------------------------------------------------- */

static MopLogCallback s_callback = NULL;
static MopLogLevel s_level = MOP_LOG_DEBUG;

/* -------------------------------------------------------------------------
 * Default callback — fprintf to stderr
 * ------------------------------------------------------------------------- */

static const char *level_str(MopLogLevel level) {
  switch (level) {
  case MOP_LOG_DEBUG:
    return "DEBUG";
  case MOP_LOG_INFO:
    return "INFO ";
  case MOP_LOG_WARN:
    return "WARN ";
  case MOP_LOG_ERROR:
    return "ERROR";
  default:
    return "?????";
  }
}

static void default_callback(MopLogLevel level, const char *file, int line,
                             const char *fmt, va_list args) {
  fprintf(stderr, "[mop:%s] %s:%d: ", level_str(level), file, line);
  vfprintf(stderr, fmt, args);
  fprintf(stderr, "\n");
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

void mop_log_set_callback(MopLogCallback cb) { s_callback = cb; }

void mop_log_set_level(MopLogLevel level) { s_level = level; }

void mop_log_emit(MopLogLevel level, const char *file, int line,
                  const char *fmt, ...) {
  if (level < s_level)
    return;

  va_list args;
  va_start(args, fmt);
  if (s_callback) {
    s_callback(level, file, line, fmt, args);
  } else {
    default_callback(level, file, line, fmt, args);
  }
  va_end(args);
}
