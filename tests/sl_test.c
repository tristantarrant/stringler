/*
 * Stringler - LV2 test harness
 *
 * Loads the plugin with the raw LV2 C API, drives the MIDI engine through
 * LV2 Events ports (this DPF revision pipes MIDI as lv2:MidiEvent atoms in
 * lv2:atom:Sequence ports, not as dedicated MIDI ports) and checks the
 * output MIDI against the expected behaviour of the recreated engine:
 *   gate, pitch transforms, scale filter, velocity normalize,
 *   octave-down companion, mono takeover, polyphonic note stealing,
 *   legato glide (deferred landing + pitch-bend ramp), bypass passthrough.
 *
 * DPF port layout (channels, not buses):
 *   0: audio in L   1: audio in R   2: audio out L   3: audio out R
 *   4: events in    5: events out
 *   6..29: control ports (24 parameters; port 29 = bypass = lv2:enabled)
 *
 * Usage: ./sl_test /path/to/stringler.lv2/stringler_dsp.so
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <dlfcn.h>

#include "lv2host.h"
#include "lv2/atom.h"
#include "lv2/midi.h"

typedef struct _LV2_Descriptor LV2_Descriptor;
typedef void *LV2_Handle;

struct _LV2_Descriptor {
  const char *plugin_uri;
  LV2_Handle (*instantiate)(const LV2_Descriptor *, double, const char *,
                            const void *const *);
  void (*connect_port)(LV2_Handle, unsigned int, void *);
  void (*activate)(LV2_Handle);
  void (*run)(LV2_Handle, unsigned int);
  void (*deactivate)(LV2_Handle);
  void (*cleanup)(LV2_Handle);
  void *reserved;
};

/* DPF port layout: audio in L/R, audio out L/R, events in/out, controls */
#define P_INL     0
#define P_INR     1
#define P_OUTL    2
#define P_OUTR    3
#define P_EVIN    4
#define P_EVOUT   5
#define P_CTRL    6
#define NCTRL     30
#define P_BYPASS  (P_CTRL + 23)

/* parameter indices (must match the Parameters enum in
   plugins/Stringler/DistrhoPluginInfo.h) */
enum { MODE, BEND, STEAL, GATE, LEGATO, LEGATOMS, VIBDEPTH, VIBRATE,
       CHORDSHIFT, CHORDWHEEL, TRANSPOSE, SCALE, ROOT, OCTDOWN, SPLIT,
       LOWSCHAN, HIGHSCHAN, BRIGHTAMT, BRIGHTCC, LOUDAMT, LOUDCC,
       VELNORM, OUTCHAN, SOURCE,
       TRACKLOW, TRACKRES, TRACKBRIGHT, TRACKJITTER, BENDTRIG, BYPASS };

#define BLOCK 1024

static int failures = 0;
static const LV2_Descriptor *desc;
static float ctrl[NCTRL];
static float inL[BLOCK], inR[BLOCK], outL[BLOCK], outR[BLOCK];

/* LV2 hosts hand the plugin a buffer whose first LV2_Atom header is
   followed by the sequence body and the events. atom.size is the usable
   capacity (buffer bytes minus the 8-byte atom header) on input, and is
   rewritten by the plugin to the written body size on output. */
#define EV_BYTES 8192
#define EV_CAP   ((uint32_t)(EV_BYTES - sizeof(LV2_Atom)))
static uint8_t evin_buf[EV_BYTES] __attribute__((aligned(8)));
static uint8_t evout_buf[EV_BYTES] __attribute__((aligned(8)));
static LV2_Atom_Sequence *evin, *evout;

static uint32_t urid_midi = 0, urid_seq = 0;

/* absolute frame of the block about to be (or last) run */
static long g_block_start = 0;

static void
check(int ok, const char *msg)
{
  printf("%s: %s\n", ok ? "PASS" : "FAIL", msg);
  if (!ok)
    ++failures;
}

/* ------------------------------------------------------------------ */
/* events-in plumbing                                                 */
/* ------------------------------------------------------------------ */

static uint32_t evin_used;

static void
evin_reset(void)
{
  evin->atom.type = urid_seq;
  evin->atom.size = sizeof(LV2_Atom_Sequence_Body);
  evin->body.unit = 0;
  evin->body.pad = 0;
  evin_used = 0;
}

/* events start right after the 8-byte sequence body header */
static void
evin_midi(uint32_t frame, const uint8_t payload[], int n)
{
  uint8_t *p = (uint8_t *)(&evin->body + 1) + evin_used;
  LV2_Atom_Event *e = (LV2_Atom_Event *)p;
  e->time.frames = (int64_t)frame;
  e->body.type = urid_midi;
  e->body.size = (uint32_t)n;
  memcpy((uint8_t *)e + sizeof(LV2_Atom_Event), payload, n);
  evin_used += sizeof(LV2_Atom_Event) + ((uint32_t)(n + 7) & ~7U);
  evin->atom.size = sizeof(LV2_Atom_Sequence_Body) + evin_used;
}

static void
evin_note(uint32_t frame, uint8_t ch, uint8_t pitch, uint8_t vel)
{
  uint8_t b[3] = { (uint8_t)((vel ? 0x90 : 0x80) | (ch & 0x0F)),
                  pitch, vel };
  evin_midi(frame, b, 3);
}

/* ------------------------------------------------------------------ */
/* events-out parsing                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
  uint32_t frame;
  int kind;   /* 0 = note on, 1 = note off, 2 = control, 3 = pitch bend */
  int ch, d1, d2;
} OutEv;

static int
parse_out(OutEv arr[], int max)
{
  int n = 0;
  /* the plugin only rewrites atom.size when it writes something; an
     untouched capacity means the run produced no MIDI */
  if (evout->atom.size <= sizeof(LV2_Atom_Sequence_Body) ||
      evout->atom.size >= EV_CAP)
    return 0;
  const uint8_t *p = (const uint8_t *)(&evout->body + 1);
  const uint8_t *end = (const uint8_t *)&evout->body + evout->atom.size;
  while (p + sizeof(LV2_Atom_Event) <= end && n < max) {
    const LV2_Atom_Event *e = (const LV2_Atom_Event *)p;
    if (e->body.type == urid_midi && e->body.size >= 2) {
      const uint8_t *pd = (const uint8_t *)e + sizeof(LV2_Atom_Event);
      OutEv *o = &arr[n];
      o->frame = (uint32_t)e->time.frames;
      /* MIDI status: command in the high nibble, channel in the low one */
      o->ch = pd[0] & 0x0F;
      switch (pd[0] & 0xF0) {
      case 0x90: o->kind = (pd[2] == 0) ? 1 : 0;   /* vel 0 = note off */
                 o->d1 = pd[1]; o->d2 = pd[2]; break;
      case 0x80: o->kind = 1; o->d1 = pd[1]; o->d2 = pd[2]; break;
      case 0xB0: o->kind = 2; o->d1 = pd[1]; o->d2 = pd[2]; break;
      case 0xE0: o->kind = 3; o->d1 = pd[1]; o->d2 = pd[2]; break;
      default:   o->kind = -1; o->d1 = 0; o->d2 = 0; break;
      }
      if (o->kind >= 0) {
        if (getenv("SL_DEBUG"))
          printf("  out: frame=%u kind=%d ch=%d d1=%d d2=%d\n",
                 o->frame, o->kind, o->ch, o->d1, o->d2);
        ++n;
      }
    }
    p += sizeof(LV2_Atom_Event) + ((e->body.size + 7) & ~7U);
  }
  return n;
}

static int
ev_find(OutEv arr[], int n, int kind, int ch, int d1, int d2)
{
  for (int i = 0; i < n; ++i)
    if (arr[i].kind == kind && arr[i].ch == ch &&
        arr[i].d1 == d1 && (d2 < 0 || arr[i].d2 == d2))
      return i;
  return -1;
}

/* ------------------------------------------------------------------ */
/* plugin lifecycle                                                   */
/* ------------------------------------------------------------------ */

static void
defaults_all_zero(void)
{
  memset(ctrl, 0, sizeof(ctrl));
  /* sensible baseline: engaged, mono, one channel, MIDI input only
     (the audio tracker has its own tests).
     NOTE: the LV2 bypass port carries the lv2:enabled designation, so
     1 = processing, 0 = bypassed (DPF inverts it for the engine). */
  ctrl[BYPASS] = 1.0f;
  ctrl[OUTCHAN] = 1.0f;
  ctrl[SOURCE] = 1.0f;
}

static LV2_Handle last_handle;

static void
open_plugin(double sr)
{
  (void)sr;
  for (int i = 0; i < NCTRL; ++i)
    desc->connect_port(last_handle, P_CTRL + i, &ctrl[i]);
  desc->connect_port(last_handle, P_EVIN, evin);
  desc->connect_port(last_handle, P_EVOUT, evout);
  desc->connect_port(last_handle, P_OUTL, outL);
  desc->connect_port(last_handle, P_OUTR, outR);
  desc->connect_port(last_handle, P_INL, inL);
  desc->connect_port(last_handle, P_INR, inR);
  desc->activate(last_handle);
  g_block_start = 0;
}

static void evout_prepare(void);

/* one process cycle, keeping absolute frame bookkeeping in step */
static void
run_block(void)
{
  evout_prepare();
  desc->run(last_handle, BLOCK);
  g_block_start += BLOCK;
}

static void
close_plugin(void)
{
  desc->deactivate(last_handle);
  desc->cleanup(last_handle);
}

static void
evout_prepare(void)
{
  memset(evout_buf, 0, sizeof(evout_buf));
  evout->atom.type = urid_seq;
  evout->atom.size = EV_CAP;   /* capacity; the plugin rewrites this */
  evout->body.unit = 0;
  evout->body.pad = 0;
}

/* ------------------------------------------------------------------ */
/* audio tracker test signal                                          */
/* ------------------------------------------------------------------ */

static double g_phase = 0.0;

/* continuous chord: one sine per MIDI note (or silence when amp <= 0) */
static void
fill_notes(const int* notes, int count, double amp)
{
  static double phase[8];
  if (amp <= 0.0) {
    memset(inL, 0, sizeof(inL));
    memset(inR, 0, sizeof(inR));
    return;
  }
  for (int i = 0; i < BLOCK; ++i) {
    double sum = 0.0;
    for (int k = 0; k < count; ++k) {
      sum += amp * sin(phase[k]);
      phase[k] += 2.0 * M_PI * 440.0 * pow(2.0, (notes[k] - 69) / 12.0) / 48000.0;
      if (phase[k] > 2.0 * M_PI)
        phase[k] -= 2.0 * M_PI;
    }
    inL[i] = inR[i] = (float)sum;
  }
}

/* continuous sine (or silence when amp <= 0) at 48 kHz */
static void
fill_audio(double freq, double amp)
{
  if (amp <= 0.0) {
    memset(inL, 0, sizeof(inL));
    memset(inR, 0, sizeof(inR));
    return;
  }
  const double w = 2.0 * M_PI * freq / 48000.0;
  for (int i = 0; i < BLOCK; ++i) {
    inL[i] = inR[i] = (float)(amp * sin(g_phase));
    g_phase += w;
    if (g_phase > 2.0 * M_PI)
      g_phase -= 2.0 * M_PI;
  }
}

int main(int argc, char **argv)
{
  if (argc < 2) {
    fprintf(stderr, "usage: %s plugin.so\n", argv[0]);
    return 1;
  }
  void *lib = dlopen(argv[1], RTLD_NOW);
  if (!lib) {
    fprintf(stderr, "cannot dlopen %s: %s\n", argv[1], dlerror());
    return 1;
  }
  const LV2_Descriptor *(*desc_fn)(unsigned int) =
    (const LV2_Descriptor *(*)(unsigned int)) dlsym(lib, "lv2_descriptor");
  desc = desc_fn ? desc_fn(0) : NULL;
  check(desc != NULL && desc->plugin_uri != NULL, "lv2_descriptor(0)");
  if (!desc)
    return 1;

  const double SR = 48000;
  evin  = (LV2_Atom_Sequence *)evin_buf;
  evout = (LV2_Atom_Sequence *)evout_buf;
  urid_midi = host_map(NULL, LV2_MIDI__MidiEvent);
  urid_seq  = host_map(NULL, LV2_ATOM__Sequence);

  defaults_all_zero();
  last_handle = desc->instantiate(desc, SR, NULL,
                                  (const void *const *)host_get_features(BLOCK));
  check(last_handle != NULL, "instantiate @48k");
  if (!last_handle)
    return 1;
  open_plugin(SR);

  /* --- 1: note on/off round trip, no transforms --------------------- */
  {
    defaults_all_zero();
    ctrl[GATE] = 0.0f;
    evin_reset();
    evout_prepare();
    evin_note(5, 0, 48, 100);
    run_block();
    OutEv ev[64];
    int n = parse_out(ev, 64);
    check(ev_find(ev, n, 0, 0, 48, 100) >= 0,
          "note on C3 vel 100 passes through (engaged, no transforms)");
    check(n == 1, "exactly one event");
  }

  /* --- 1b: audio tracker: a sine produces a MIDI note --------------- */
  {
    defaults_all_zero();
    ctrl[SOURCE] = 0.0f;   /* audio only */
    ctrl[GATE] = 0.0f;
    g_phase = 0.0;
    int found = 0, vel = 0;
    OutEv ev[64];
    for (int b = 0; b < 10 && !found; ++b) {
      fill_audio(220.0, 0.30);
      evin_reset();
      evout_prepare();
      run_block();
      const int n = parse_out(ev, 64);
      const int i = ev_find(ev, n, 0, 0, 57, -1);
      if (i >= 0) {
        found = 1;
        vel = ev[i].d2;
      }
    }
    check(found, "audio tracker: 220 Hz sine triggers MIDI note A3 (57)");
    check(found && vel > 20 && vel < 127,
          "audio tracker: velocity derived from the input level");

    int released = 0;
    for (int b = 0; b < 6 && !released; ++b) {
      fill_audio(0.0, 0.0);
      evin_reset();
      evout_prepare();
      run_block();
      const int n = parse_out(ev, 64);
      if (ev_find(ev, n, 1, 0, 57, -1) >= 0)
        released = 1;
    }
    check(released, "audio tracker: silence releases the note");
  }

  /* --- 1c: below the gate nothing triggers -------------------------- */
  {
    defaults_all_zero();
    ctrl[SOURCE] = 0.0f;
    ctrl[GATE] = 0.5f;
    int notes = 0;
    OutEv ev[64];
    for (int b = 0; b < 8; ++b) {
      fill_audio(220.0, 0.002);
      evin_reset();
      evout_prepare();
      run_block();
      notes += parse_out(ev, 64);
    }
    check(notes == 0, "audio tracker: quiet input stays below the gate");
  }

  /* --- 1d: source routing ------------------------------------------- */
  {
    defaults_all_zero();
    ctrl[SOURCE] = 1.0f;   /* MIDI only: audio ignored */
    int notes = 0;
    OutEv ev[64];
    for (int b = 0; b < 8; ++b) {
      fill_audio(220.0, 0.30);
      evin_reset();
      evout_prepare();
      run_block();
      notes += parse_out(ev, 64);
    }
    check(notes == 0, "source = MIDI: audio input is ignored");

    defaults_all_zero();
    ctrl[SOURCE] = 2.0f;   /* both */
    int midi = 0, audio = 0;
    for (int b = 0; b < 10; ++b) {
      fill_audio(220.0, 0.30);
      evin_reset();
      evout_prepare();
      if (b == 0)
        evin_note(0, 0, 48, 100);
      run_block();
      const int n = parse_out(ev, 64);
      if (ev_find(ev, n, 0, 0, 48, 100) >= 0)
        midi = 1;
      if (ev_find(ev, n, 0, 0, 57, -1) >= 0)
        audio = 1;
    }
    check(midi && audio, "source = both: MIDI and audio notes both pass");
  }

  /* --- 1e: pitch accuracy across the guitar range ------------------- */
  {
    static const struct { double f; int note; } cases[] = {
      { 82.41, 40 },    /* E2 */
      { 110.00, 45 },   /* A2 */
      { 146.83, 50 },   /* D3 */
      { 220.00, 57 },   /* A3 */
      { 329.63, 64 },   /* E4 */
    };
    int okAll = 1;
    OutEv ev[64];
    for (unsigned k = 0; k < sizeof(cases) / sizeof(cases[0]); ++k) {
      defaults_all_zero();
      ctrl[SOURCE] = 0.0f;
      ctrl[GATE] = 0.0f;
      g_phase = 0.0;
      int found = 0;
      for (int b = 0; b < 12 && !found; ++b) {
        fill_audio(cases[k].f, 0.30);
        evin_reset();
        evout_prepare();
        run_block();
        const int n = parse_out(ev, 64);
        if (ev_find(ev, n, 0, 0, cases[k].note, -1) >= 0)
          found = 1;
      }
      if (!found) {
        printf("  pitch miss: %.2f Hz expected note %d\n", cases[k].f, cases[k].note);
        okAll = 0;
      }
      for (int b = 0; b < 3; ++b) {
        fill_audio(0.0, 0.0);
        evin_reset();
        evout_prepare();
        run_block();
      }
    }
    check(okAll, "audio tracker: correct notes across the guitar range (E2..E4)");
  }

  /* --- 1f: polyphonic audio tracking (MODE = POLY) ------------------ */
  {
    static const int notes[3] = { 45, 50, 54 };   /* A2, D3, F#3 */
    defaults_all_zero();
    ctrl[SOURCE] = 0.0f;
    ctrl[GATE] = 0.0f;
    ctrl[MODE] = 1.0f;
    ctrl[STEAL] = 0.0f;
    int got[3] = { 0, 0, 0 };
    OutEv ev[64];
    for (int b = 0; b < 16; ++b) {
      fill_notes(notes, 3, 0.15);
      evin_reset();
      evout_prepare();
      run_block();
      const int n = parse_out(ev, 64);
      for (int k = 0; k < 3; ++k)
        if (ev_find(ev, n, 0, 0, notes[k], -1) >= 0)
          got[k] = 1;
    }
    check(got[0] && got[1] && got[2],
          "audio tracker (poly): three-note chord detected");

    int released = 0;
    for (int b = 0; b < 8 && released < 3; ++b) {
      fill_notes(notes, 0, 0.0);
      evin_reset();
      evout_prepare();
      run_block();
      const int n = parse_out(ev, 64);
      for (int k = 0; k < 3; ++k)
        if (ev_find(ev, n, 1, 0, notes[k], -1) >= 0)
          ++released;
    }
    check(released >= 3, "audio tracker (poly): chord released on silence");

    /* four non-harmonically-related notes */
    {
      static const int four[4] = { 45, 48, 51, 54 };   /* A2, C3, D#3, F#3 */
      defaults_all_zero();
      ctrl[SOURCE] = 0.0f;
      ctrl[GATE] = 0.0f;
      ctrl[MODE] = 1.0f;
      ctrl[STEAL] = 0.0f;
      int got4 = 0;
      OutEv e2[64];
      for (int b = 0; b < 20 && got4 != 15; ++b) {
        fill_notes(four, 4, 0.12);
        evin_reset();
        evout_prepare();
        run_block();
        const int n = parse_out(e2, 64);
        for (int k = 0; k < 4; ++k)
          if (ev_find(e2, n, 0, 0, four[k], -1) >= 0)
            got4 |= 1 << k;
      }
      check(got4 == 15, "audio tracker (poly): four-note chord detected");
    }
  }

  /* --- 1g: LOWEST NOTE limits the tracker range --------------------- */
  {
    defaults_all_zero();
    ctrl[SOURCE] = 0.0f;
    ctrl[GATE] = 0.0f;
    g_phase = 0.0;
    OutEv ev[64];
    ctrl[TRACKLOW] = 45.0f;   /* A2: E2 is below the tracker range */
    int notes = 0;
    for (int b = 0; b < 12; ++b) {
      fill_audio(82.41, 0.30);
      evin_reset();
      evout_prepare();
      run_block();
      const int n = parse_out(ev, 64);
      for (int i = 0; i < n; ++i)
        if (ev[i].kind == 0)
          ++notes;
    }
    check(notes == 0, "audio tracker: notes below LOWEST NOTE are ignored");

    ctrl[TRACKLOW] = 40.0f;   /* E2 accepted again */
    int found = 0;
    for (int b = 0; b < 12 && !found; ++b) {
      fill_audio(82.41, 0.30);
      evin_reset();
      evout_prepare();
      run_block();
      const int n = parse_out(ev, 64);
      if (ev_find(ev, n, 0, 0, 40, -1) >= 0)
        found = 1;
    }
    check(found, "audio tracker: lowering LOWEST NOTE re-enables E2");
  }

  /* --- 1h: continuous pitch becomes a bend (MONO) ------------------- */
  {
    defaults_all_zero();
    ctrl[SOURCE] = 0.0f;
    ctrl[GATE] = 0.0f;
    ctrl[BEND] = 12.0f;   /* bends need a configured wheel range */
    ctrl[BENDTRIG] = 50.0f;
    g_phase = 0.0;
    OutEv ev[64];
    int noted = 0;
    for (int b = 0; b < 10 && !noted; ++b) {
      fill_audio(220.0, 0.30);
      evin_reset();
      evout_prepare();
      run_block();
      const int n = parse_out(ev, 64);
      if (ev_find(ev, n, 0, 0, 57, -1) >= 0)
        noted = 1;
    }
    check(noted, "bend: A3 is tracked first");

    int up = 0, retrigger = 0;
    for (int b = 0; b < 8; ++b) {
      fill_audio(233.08, 0.30);   /* +1 semitone */
      evin_reset();
      evout_prepare();
      run_block();
      const int n = parse_out(ev, 64);
      for (int i = 0; i < n; ++i) {
        if (ev[i].kind == 3 && ((ev[i].d2 << 7) | ev[i].d1) > 8292)
          up = 1;
        if (ev[i].kind == 0 && ev[i].d1 == 58)
          retrigger = 1;
      }
    }
    check(up, "bend: a one-semitone rise emits an upward bend");
    check(!retrigger, "bend: no retrigger inside the bend window");

    int down = 0;
    for (int b = 0; b < 8; ++b) {
      fill_audio(207.65, 0.30);   /* -1 semitone */
      evin_reset();
      evout_prepare();
      run_block();
      const int n = parse_out(ev, 64);
      for (int i = 0; i < n; ++i)
        if (ev[i].kind == 3 && ((ev[i].d2 << 7) | ev[i].d1) < 8092)
          down = 1;
    }
    check(down, "bend: a one-semitone fall emits a downward bend");
  }

  /* --- 1i: BEND/TRIG balance --------------------------------------- */
  {
    OutEv ev[64];

    /* triggers: any deliberate movement becomes a new note */
    defaults_all_zero();
    ctrl[SOURCE] = 0.0f;
    ctrl[GATE] = 0.0f;
    ctrl[BEND] = 12.0f;
    ctrl[BENDTRIG] = 0.0f;
    g_phase = 0.0;
    int noted = 0;
    for (int b = 0; b < 10 && !noted; ++b) {
      fill_audio(220.0, 0.30);
      evin_reset();
      evout_prepare();
      run_block();
      const int n = parse_out(ev, 64);
      if (ev_find(ev, n, 0, 0, 57, -1) >= 0)
        noted = 1;
    }
    int retrig = 0;
    for (int b = 0; b < 10; ++b) {
      fill_audio(233.08, 0.30);
      evin_reset();
      evout_prepare();
      run_block();
      const int n = parse_out(ev, 64);
      if (ev_find(ev, n, 0, 0, 58, -1) >= 0)
        retrig = 1;
    }
    check(noted && retrig, "bend/trig 0%: a semitone change retriggers");

    /* release and settle */
    defaults_all_zero();
    for (int b = 0; b < 4; ++b) {
      fill_audio(0.0, 0.0);
      evin_reset();
      evout_prepare();
      run_block();
    }

    /* bends: the same movement stays a bend */
    defaults_all_zero();
    ctrl[SOURCE] = 0.0f;
    ctrl[GATE] = 0.0f;
    ctrl[BEND] = 12.0f;
    ctrl[BENDTRIG] = 100.0f;
    g_phase = 0.0;
    noted = 0;
    for (int b = 0; b < 10 && !noted; ++b) {
      fill_audio(220.0, 0.30);
      evin_reset();
      evout_prepare();
      run_block();
      const int n = parse_out(ev, 64);
      if (ev_find(ev, n, 0, 0, 57, -1) >= 0)
        noted = 1;
    }
    int up = 0, retrig2 = 0;
    for (int b = 0; b < 8; ++b) {
      fill_audio(233.08, 0.30);
      evin_reset();
      evout_prepare();
      run_block();
      const int n = parse_out(ev, 64);
      for (int i = 0; i < n; ++i) {
        if (ev[i].kind == 3 && ((ev[i].d2 << 7) | ev[i].d1) > 8292)
          up = 1;
        if (ev[i].kind == 0 && ev[i].d1 == 58)
          retrig2 = 1;
      }
    }
    check(noted && up && !retrig2,
          "bend/trig 100%: the same movement bends instead");
  }

  /* --- 2: octave-down companion at half velocity -------------------- */
  {
    defaults_all_zero();
    ctrl[GATE] = 0.0f;
    ctrl[OCTDOWN] = 0.5f;
    evin_reset();
    evout_prepare();
    evin_note(0, 0, 48, 100);
    run_block();
    OutEv ev[64];
    int n = parse_out(ev, 64);
    check(ev_find(ev, n, 0, 0, 48, 100) >= 0, "root note on C3");
    check(ev_find(ev, n, 0, 0, 36, 50) >= 0,
          "octave-down companion C2 at half velocity");
    /* release both */
    evin_reset();
    evout_prepare();
    evin_note(0, 0, 48, 0);
    run_block();
    n = parse_out(ev, 64);
    check(ev_find(ev, n, 1, 0, 48, 0) >= 0 &&
          ev_find(ev, n, 1, 0, 36, 0) >= 0,
          "note off releases root and companion");
  }

  /* --- 2b: output channel and split routing ------------------------- */
  {
    OutEv ev[64];

    defaults_all_zero();
    ctrl[OUTCHAN] = 3.0f;
    ctrl[GATE] = 0.0f;
    evin_reset();
    evout_prepare();
    evin_note(0, 0, 48, 100);
    run_block();
    int n = parse_out(ev, 64);
    check(ev_find(ev, n, 0, 2, 48, 100) >= 0,
          "out chan 3: note on goes out on wire channel 2");

    evin_reset();
    evout_prepare();
    evin_note(0, 0, 48, 0);
    run_block();
    n = parse_out(ev, 64);
    check(ev_find(ev, n, 1, 2, 48, 0) >= 0,
          "out chan 3: note off on wire channel 2");

    defaults_all_zero();
    ctrl[SPLIT] = 48.0f;   /* C3 */
    ctrl[LOWSCHAN] = 2.0f;
    ctrl[HIGHSCHAN] = 5.0f;
    ctrl[GATE] = 0.0f;
    evin_reset();
    evout_prepare();
    evin_note(0, 0, 40, 100);
    run_block();
    n = parse_out(ev, 64);
    check(ev_find(ev, n, 0, 1, 40, 100) >= 0,
          "split: a note below C3 goes to Low Ch (2)");
    evin_reset();
    evout_prepare();
    evin_note(0, 0, 40, 0);
    run_block();

    evin_reset();
    evout_prepare();
    evin_note(0, 0, 60, 100);
    run_block();
    n = parse_out(ev, 64);
    check(ev_find(ev, n, 0, 4, 60, 100) >= 0,
          "split: a note above C3 goes to High Ch (5)");
  }

  /* --- 3: gate rejects weak notes ------------------------------------ */
  {
    defaults_all_zero();
    ctrl[GATE] = 0.5f;
    evin_reset();
    evout_prepare();
    evin_note(0, 0, 48, 50);
    run_block();
    OutEv ev[64];
    int n = parse_out(ev, 64);
    check(n == 0, "gate 0.5: vel 50 (< 63.5) is swallowed");

    evin_reset();
    evout_prepare();
    evin_note(0, 0, 48, 100);
    run_block();
    n = parse_out(ev, 64);
    check(ev_find(ev, n, 0, 0, 48, 100) >= 0,
          "gate 0.5: vel 100 gets through");
  }

  /* --- 4: transpose --------------------------------------------------- */
  {
    defaults_all_zero();
    ctrl[TRANSPOSE] = 2.0f;
    evin_reset();
    evout_prepare();
    evin_note(0, 0, 48, 100);
    run_block();
    OutEv ev[64];
    int n = parse_out(ev, 64);
    check(ev_find(ev, n, 0, 0, 50, 100) >= 0, "transpose +2 shifts C3 to D3");
    ctrl[TRANSPOSE] = 0.0f;
  }

  /* --- 5: scale filter (major, root C) -------------------------------- */
  {
    defaults_all_zero();
    ctrl[SCALE] = 1.0f;
    ctrl[ROOT] = 0.0f;
    evin_reset();
    evout_prepare();
    evin_note(0, 0, 49, 100);   /* Db: not in C major */
    run_block();
    OutEv ev[64];
    int n = parse_out(ev, 64);
    check(n == 0, "scale filter swallows Db (not in C major)");

    evin_reset();
    evout_prepare();
    evin_note(0, 0, 50, 100);   /* D: in C major */
    run_block();
    n = parse_out(ev, 64);
    check(ev_find(ev, n, 0, 0, 50, 100) >= 0,
          "scale filter passes D (in C major)");
    ctrl[SCALE] = 0.0f;
  }

  /* --- 6: velocity normalize ------------------------------------------ */
  {
    defaults_all_zero();
    ctrl[VELNORM] = 100.0f;
    evin_reset();
    evout_prepare();
    evin_note(0, 0, 48, 64);
    run_block();
    OutEv ev[64];
    int n = parse_out(ev, 64);
    check(ev_find(ev, n, 0, 0, 48, 50) >= 0,
          "velnorm 100 scales vel 64 to 50 (64*100/127)");
    ctrl[VELNORM] = 0.0f;
  }

  /* --- 7: bypass = untouched passthrough ------------------------------ */
  {
    defaults_all_zero();
    /* release whatever the previous test left sounding: the bypass
       transition flushes voices, which would add note-offs to this block */
    evin_reset();
    for (int p = 36; p < 72; ++p)
      evin_note(0, 0, (uint8_t)p, 0);
    run_block();
    ctrl[TRANSPOSE] = 2.0f;
    ctrl[BYPASS] = 0.0f;   /* lv2:enabled = 0 -> bypassed */
    evin_reset();
    evout_prepare();
    evin_note(0, 0, 48, 100);
    run_block();
    OutEv ev[64];
    int n = parse_out(ev, 64);
    check(ev_find(ev, n, 0, 0, 48, 100) >= 0 && n == 1,
          "bypassed: raw passthrough despite transpose=2");
    ctrl[BYPASS] = 1.0f;
    ctrl[TRANSPOSE] = 0.0f;
  }

  /* --- 8: polyphonic note stealing ------------------------------------- */
  {
    defaults_all_zero();
    ctrl[MODE] = 1.0f;
    ctrl[STEAL] = 1.0f;
    ctrl[BEND] = 0.0f;   /* no glide: hard retrigger */
    evin_reset();
    evout_prepare();
    evin_note(0, 0, 48, 100);
    run_block();
    OutEv ev[64];
    int n = parse_out(ev, 64);
    check(ev_find(ev, n, 0, 0, 48, 100) >= 0, "poly: C3 sounds");

    evin_reset();
    evout_prepare();
    evin_note(10, 0, 49, 100);   /* within steal range 1 */
    run_block();
    n = parse_out(ev, 64);
    check(ev_find(ev, n, 1, 0, 48, 0) >= 0,
          "steal: C3 released when Db3 arrives");
    check(ev_find(ev, n, 0, 0, 49, 100) >= 0,
          "steal: Db3 takes over the voice");
  }

  /* --- 9: legato glide with deferred landing --------------------------- */
  {
    defaults_all_zero();
    ctrl[MODE] = 0.0f;
    ctrl[BEND] = 12.0f;
    ctrl[LEGATO] = 1.0f;
    ctrl[LEGATOMS] = 30.0f;
    ctrl[STEAL] = 1.0f;
    evin_reset();
    evout_prepare();
    evin_note(0, 0, 48, 100);
    run_block();

    /* second note 100 frames later: attacks at the old pitch */
    evin_reset();
    evout_prepare();
    evin_note(100, 0, 52, 100);
    const long note_start = g_block_start;   /* absolute frame of this block */
    run_block();
    OutEv ev[64];
    int n = parse_out(ev, 64);
    check(ev_find(ev, n, 0, 0, 48, 100) >= 0,
          "glide: attack on the old pitch (C3) at frame 100");

    /* the glide lasts 30 ms = 1440 frames; watch it land */
    OutEv land[64];
    int nl = 0;
    long land_start = 0;
    for (int b = 0; b < 4; ++b) {
      evin_reset();
      evout_prepare();
      const long start = g_block_start;
      run_block();
      nl = parse_out(land, 64);
      if (nl > 0) {
        land_start = start;
        break;
      }
    }
    const long exp_land = note_start + 100 + 1440;
    check(ev_find(land, nl, 1, 0, 48, 0) >= 0,
          "glide: old pitch released at the landing");
    check(ev_find(land, nl, 0, 0, 52, 100) >= 0,
          "glide: new pitch (E3) sounds at the landing");
    {
      /* the landing happened at the expected moment (one block jitter) */
      int ok_frame = 0;
      for (int i = 0; i < nl; ++i)
        if (land[i].kind == 0 && land[i].d1 == 52) {
          const long abs = land_start + (long)land[i].frame;
          if (abs >= exp_land && abs < exp_land + BLOCK)
            ok_frame = 1;
        }
      check(ok_frame, "glide: landing frame matches 30 ms duration");
    }
  }

  /* --- 10: fuzz - random notes, random parameters ---------------------- */
  {
    srand(1);
    int stable = 1;
    for (int b = 0; b < 4096 && stable; ++b) {
      if (b % 64 == 0) {
        for (int i = 0; i < NCTRL; ++i) {
          float v = (float)(rand() % 1001) / 100.0f;
          /* keep values inside each parameter's range */
          switch (i) {
          case MODE: case LEGATO: case BYPASS: v = rand() % 2; break;
          case BEND: v *= 48.0f; break;
          case STEAL: v *= 12.0f; break;
          case GATE: v /= 10.0f; break;
          case LEGATOMS: v *= 100.0f; break;
          case VIBDEPTH: v *= 200.0f; break;
          case VIBRATE: v *= 10.0f; break;
          case CHORDSHIFT: v = v * 24.0f - 12.0f; break;
          case CHORDWHEEL: v *= 3.0f; break;
          case TRANSPOSE: v = v * 48.0f - 24.0f; break;
          case SCALE: v *= 17.0f; break;
          case ROOT: v *= 11.0f; break;
          case OCTDOWN: v *= 2.0f; break;
          case SPLIT: v *= 64.0f; break;
          case LOWSCHAN: case HIGHSCHAN: case OUTCHAN:
            v = 1.0f + v * 15.0f; break;
          case BRIGHTAMT: case LOUDAMT: v *= 2.0f; break;
          case BRIGHTCC: case LOUDCC: case VELNORM: v *= 127.0f; break;
          }
          ctrl[i] = v;
        }
      }
      for (int i = 0; i < BLOCK; ++i)
        inL[i] = inR[i] = (rand() % 2001 / 1000.0f - 1.0f) * 0.5f;

      evin_reset();
      evout_prepare();
      for (int k = 0; k < 8; ++k) {
        uint8_t on = rand() % 2;
        evin_note(rand() % BLOCK, rand() % 16, rand() % 96 + 24,
                 on ? (rand() % 127 + 1) : 0);
      }
      run_block();

      OutEv ev[512];
      int n = parse_out(ev, 512);
      for (int i = 0; i < n; ++i) {
        if (ev[i].kind < 0 || ev[i].d1 > 127 ||
            (ev[i].kind <= 1 && ev[i].d2 > 127)) {
          stable = 0;
          break;
        }
      }
      for (int i = 0; i < BLOCK; ++i)
        if (!isfinite(outL[i]) || fabsf(outL[i]) > 2.0f)
          stable = 0;
    }
    check(stable, "4096 blocks random notes + random parameters: well-formed, finite");
  }

  /* --- 11: other sample rates ------------------------------------------- */
  {
    close_plugin();
    const double rates[] = { 44100, 88200, 96000 };
    for (int i = 0; i < 3; ++i) {
      defaults_all_zero();
      last_handle = desc->instantiate(desc, rates[i], NULL,
                                     (const void *const *)host_get_features(BLOCK));
      if (!last_handle) {
        check(0, "instantiate at other rate");
        break;
      }
      open_plugin(rates[i]);
      evin_reset();
      evout_prepare();
      evin_note(0, 0, 48, 100);
      run_block();
      OutEv ev[64];
      int n = parse_out(ev, 64);
      char msg[128];
      snprintf(msg, sizeof msg, "@%.0f Hz: note on passes through", rates[i]);
      check(ev_find(ev, n, 0, 0, 48, 100) >= 0, msg);
      close_plugin();
    }
  }

  dlclose(lib);
  printf("\n%s (%d failure%s)\n", failures ? "TESTS FAILED" : "ALL TESTS PASSED",
         failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
