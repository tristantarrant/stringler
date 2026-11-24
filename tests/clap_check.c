/*
 * CLAP smoke test: load the .clap, create the plugin through the factory,
 * activate it, process a block of audio and list its parameters.
 *
 * usage: clap_check <plugin.clap> <expected-id> [param-count]
 *
 * Copyright (C) 2026 Dataforte
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clap/entry.h"
#include "clap/plugin-factory.h"
#include "clap/ext/params.h"
#include "clap/ext/gui.h"

static int failures = 0;
static void check(int ok, const char *msg)
{
  printf("%s: %s\n", ok ? "PASS" : "FAIL", msg);
  if (!ok)
    ++failures;
}

static const void *host_get_extension(const clap_host_t *h, const char *id) { (void)h; (void)id; return NULL; }
static void host_noop(const clap_host_t *h) { (void)h; }

static const clap_host_t host = {
  CLAP_VERSION_INIT, NULL, "clap_check", "Dataforte", "", "1.0",
  host_get_extension, host_noop, host_noop, host_noop,
};

static uint32_t in_size(const clap_input_events_t *l) { (void)l; return 0; }
static const clap_event_header_t *in_get(const clap_input_events_t *l, uint32_t i) { (void)l; (void)i; return NULL; }
static bool out_push(const clap_output_events_t *l, const clap_event_header_t *e) { (void)l; (void)e; return true; }

int main(int argc, char **argv)
{
  if (argc < 3) {
    fprintf(stderr, "usage: %s plugin.clap id [param-count]\n", argv[0]);
    return 2;
  }
  void *lib = dlopen(argv[1], RTLD_NOW);
  check(lib != NULL, "dlopen");
  if (!lib) { fprintf(stderr, "%s\n", dlerror()); return 1; }
  const clap_plugin_entry_t *entry = dlsym(lib, "clap_entry");
  check(entry && entry->init(argv[1]), "clap_entry init");
  const clap_plugin_factory_t *fac = entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
  check(fac && fac->get_plugin_count(fac) == 1, "factory with one plugin");
  const clap_plugin_descriptor_t *d = fac->get_plugin_descriptor(fac, 0);
  char msg[256];
  snprintf(msg, sizeof msg, "descriptor id %s, name \"%s\", vendor \"%s\"", d->id, d->name, d->vendor);
  check(strcmp(d->id, argv[2]) == 0, msg);

  const clap_plugin_t *p = fac->create_plugin(fac, &host, d->id);
  check(p && p->init(p), "create + init");
  check(p->activate(p, 48000, 32, 1024), "activate @48k");
  check(p->start_processing(p), "start_processing");

  /* the plugin has one stereo bus on each side (two channels) */
  static float inL[1024], inR[1024], outL[1024], outR[1024];
  for (int i = 0; i < 1024; ++i) {
    inL[i] = 0.3f * sinf(2.0f * 3.14159265f * 440.0f * i / 48000.0f);
    inR[i] = inL[i];
  }
  float *inp[2] = { inL, inR }, *outp[2] = { outL, outR };
  clap_audio_buffer_t ain = { inp, NULL, 2, 0, 0 }, aout = { outp, NULL, 2, 0, 0 };
  clap_input_events_t ievs = { NULL, in_size, in_get };
  clap_output_events_t oevs = { NULL, out_push };
  clap_process_t proc = { 0, 1024, NULL, &ain, &aout, 1, 1, &ievs, &oevs };
  double acc = 0;
  int finite = 1;
  for (int b = 0; b < 20; ++b) {
    p->process(p, &proc);
    for (int i = 0; i < 1024; ++i) {
      finite &= isfinite(outL[i]) && isfinite(outR[i]);
      acc += (double)outL[i] * outL[i] + (double)outR[i] * outR[i];
    }
  }
  snprintf(msg, sizeof msg, "process: finite output, rms %.3f", sqrt(acc / (20 * 2048)));
  check(finite && acc > 0, msg);

  const clap_plugin_params_t *params = p->get_extension(p, CLAP_EXT_PARAMS);
  if (params) {
    const uint32_t n = params->count(p);
    printf("      parameters:");
    for (uint32_t i = 0; i < n; ++i) {
      clap_param_info_t info;
      if (params->get_info(p, i, &info))
        printf(" %s[%g..%g=%g]", info.name, info.min_value, info.max_value, info.default_value);
    }
    printf("\n");
    if (argc > 3) {
      snprintf(msg, sizeof msg, "%u parameters (expected %s)", n, argv[3]);
      check(n == (uint32_t)atoi(argv[3]), msg);
    }
  } else {
    check(0, "params extension");
  }
  check(p->get_extension(p, CLAP_EXT_GUI) != NULL, "gui extension present");

  p->stop_processing(p);
  p->deactivate(p);
  p->destroy(p);
  entry->deinit();
  printf("\n%s (%d failure%s)\n", failures ? "CLAP TESTS FAILED" : "ALL CLAP TESTS PASSED",
         failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
