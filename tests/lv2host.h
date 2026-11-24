/*
 * Minimal LV2 host features for the C test harnesses: the urid:map and
 * options (maxBlockLength) features that DPF-based plugins require.
 *
 * Copyright (C) 2026 Dataforte
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef TEST_LV2HOST_H
#define TEST_LV2HOST_H

#include <stdint.h>
#include <string.h>

typedef struct { const char *URI; void *data; } LV2_Feature;

typedef uint32_t LV2_URID;
typedef struct {
  void *handle;
  LV2_URID (*map)(void *handle, const char *uri);
} LV2_URID_Map;

typedef struct {
  uint32_t context, subject;
  LV2_URID key;
  uint32_t size;
  LV2_URID type;
  const void *value;
} LV2_Options_Option;

#define HOST_MAX_URIS 64
static const char *host_uris[HOST_MAX_URIS];
static uint32_t host_nuris = 0;

static LV2_URID
host_map(void *handle, const char *uri)
{
  (void)handle;
  for (uint32_t i = 0; i < host_nuris; ++i)
    if (strcmp(host_uris[i], uri) == 0)
      return i + 1;
  if (host_nuris == HOST_MAX_URIS)
    return 0;
  host_uris[host_nuris++] = uri;
  return host_nuris;
}

static LV2_URID_Map host_urid_map = { NULL, host_map };
static int32_t host_block_length;
static LV2_Options_Option host_options[2];
static LV2_Feature host_f_map = { "http://lv2plug.in/ns/ext/urid#map", &host_urid_map };
static LV2_Feature host_f_opts = { "http://lv2plug.in/ns/ext/options#options", host_options };
static const LV2_Feature *host_features[] = { &host_f_map, &host_f_opts, NULL };

/* features for instantiate(); block_length = largest run() size */
static const LV2_Feature *const *
host_get_features(int32_t block_length)
{
  host_block_length = block_length;
  memset(host_options, 0, sizeof(host_options));
  host_options[0].key = host_map(NULL, "http://lv2plug.in/ns/ext/buf-size#maxBlockLength");
  host_options[0].size = sizeof(int32_t);
  host_options[0].type = host_map(NULL, "http://lv2plug.in/ns/ext/atom#Int");
  host_options[0].value = &host_block_length;
  return (const LV2_Feature *const *)host_features;
}

#endif /* TEST_LV2HOST_H */
