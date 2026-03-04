/*
 * Master of Puppets — Conformance Framework
 * perf_baseline.c — Performance regression detection
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "perf_baseline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Simple JSON parsing helpers
 *
 * We parse a flat JSON object with string and number values.  No nested
 * objects, no arrays.  This is intentionally minimal to avoid pulling in
 * a JSON library dependency.
 * ------------------------------------------------------------------------- */

/* Skip whitespace, return pointer to first non-ws char or end of string. */
static const char *skip_ws(const char *p) {
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
    p++;
  return p;
}

/* Parse a JSON string value starting after the opening quote.
 * Writes into `buf` up to `buf_size - 1` chars.  Returns pointer past
 * the closing quote, or NULL on error. */
static const char *parse_json_string(const char *p, char *buf,
                                     size_t buf_size) {
  size_t i = 0;
  while (*p && *p != '"') {
    if (*p == '\\' && *(p + 1)) {
      p++; /* skip escape */
    }
    if (i < buf_size - 1)
      buf[i++] = *p;
    p++;
  }
  buf[i] = '\0';
  if (*p == '"')
    p++;
  return p;
}

/* Extract a string value for a given key from the JSON text.
 * Returns true if found. */
static bool json_get_string(const char *json, const char *key, char *out,
                            size_t out_size) {
  char needle[256];
  snprintf(needle, sizeof(needle), "\"%s\"", key);
  const char *pos = strstr(json, needle);
  if (!pos)
    return false;
  pos += strlen(needle);
  pos = skip_ws(pos);
  if (*pos != ':')
    return false;
  pos++;
  pos = skip_ws(pos);
  if (*pos != '"')
    return false;
  pos++;
  parse_json_string(pos, out, out_size);
  return true;
}

/* Extract a number value for a given key from the JSON text.
 * Returns true if found. */
static bool json_get_number(const char *json, const char *key, double *out) {
  char needle[256];
  snprintf(needle, sizeof(needle), "\"%s\"", key);
  const char *pos = strstr(json, needle);
  if (!pos)
    return false;
  pos += strlen(needle);
  pos = skip_ws(pos);
  if (*pos != ':')
    return false;
  pos++;
  pos = skip_ws(pos);
  char *end = NULL;
  *out = strtod(pos, &end);
  return (end != pos);
}

/* -------------------------------------------------------------------------
 * Load baseline from JSON file
 * ------------------------------------------------------------------------- */

bool mop_conf_perf_load_baseline(const char *path, MopConfPerfBaseline *out) {
  if (!path || !out)
    return false;

  memset(out, 0, sizeof(*out));

  FILE *f = fopen(path, "r");
  if (!f)
    return false;

  /* Read entire file */
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);

  if (len <= 0 || len > 1024 * 1024) {
    fclose(f);
    return false;
  }

  char *buf = (char *)malloc((size_t)len + 1);
  if (!buf) {
    fclose(f);
    return false;
  }

  size_t nread = fread(buf, 1, (size_t)len, f);
  fclose(f);
  buf[nread] = '\0';

  /* Parse fields */
  json_get_string(buf, "gpu_name", out->gpu_name, sizeof(out->gpu_name));
  json_get_string(buf, "driver_version", out->driver_version,
                  sizeof(out->driver_version));
  json_get_number(buf, "frame_ms_1m", &out->frame_ms_1m);
  json_get_number(buf, "frame_ms_5m", &out->frame_ms_5m);
  json_get_number(buf, "frame_ms_10m", &out->frame_ms_10m);

  free(buf);
  return true;
}

/* -------------------------------------------------------------------------
 * Save baseline to JSON file
 * ------------------------------------------------------------------------- */

bool mop_conf_perf_save_baseline(const char *path,
                                 const MopConfPerfBaseline *baseline) {
  if (!path || !baseline)
    return false;

  FILE *f = fopen(path, "w");
  if (!f)
    return false;

  fprintf(f, "{\n");
  fprintf(f, "  \"gpu_name\": \"%s\",\n", baseline->gpu_name);
  fprintf(f, "  \"driver_version\": \"%s\",\n", baseline->driver_version);
  fprintf(f, "  \"frame_ms_1m\": %.4f,\n", baseline->frame_ms_1m);
  fprintf(f, "  \"frame_ms_5m\": %.4f,\n", baseline->frame_ms_5m);
  fprintf(f, "  \"frame_ms_10m\": %.4f\n", baseline->frame_ms_10m);
  fprintf(f, "}\n");

  fclose(f);
  return true;
}

/* -------------------------------------------------------------------------
 * Compare current timings against baseline
 *
 * Returns true if ALL current timings are within (baseline * (1 + threshold))
 * i.e., no single triangle-count tier has regressed beyond the threshold.
 * A threshold_ratio of 0.15 means 15% regression is tolerated.
 * ------------------------------------------------------------------------- */

bool mop_conf_perf_check_regression(const MopConfPerfBaseline *baseline,
                                    double current_1m, double current_5m,
                                    double current_10m,
                                    double threshold_ratio) {
  if (!baseline)
    return false;

  /* Skip comparison for tiers where baseline is zero (not measured) */
  if (baseline->frame_ms_1m > 0.0) {
    double limit = baseline->frame_ms_1m * (1.0 + threshold_ratio);
    if (current_1m > limit)
      return false;
  }

  if (baseline->frame_ms_5m > 0.0) {
    double limit = baseline->frame_ms_5m * (1.0 + threshold_ratio);
    if (current_5m > limit)
      return false;
  }

  if (baseline->frame_ms_10m > 0.0) {
    double limit = baseline->frame_ms_10m * (1.0 + threshold_ratio);
    if (current_10m > limit)
      return false;
  }

  return true;
}
