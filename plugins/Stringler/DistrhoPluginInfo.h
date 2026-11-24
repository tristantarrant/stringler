/*
 * Stringler - guitar MIDI: turns MIDI notes into expressive guitar MIDI
 * (legato glides, vibrato, chord shifting, scale filtering, CC dynamics).
 * Functional recreation of the core tracking/expression pipeline of a
 * commercial guitar-MIDI plugin (see README, "Research" section).
 *
 * Copyright (C) 2026 Dataforte
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef DISTRHO_PLUGIN_INFO_H_INCLUDED
#define DISTRHO_PLUGIN_INFO_H_INCLUDED

#define DISTRHO_PLUGIN_BRAND   "Dataforte"
#define DISTRHO_PLUGIN_NAME    "Stringler"
#define DISTRHO_PLUGIN_URI     "http://dataforte.net/plugins/stringler"
#define DISTRHO_PLUGIN_CLAP_ID "net.dataforte.stringler"

#define DISTRHO_PLUGIN_BRAND_ID  Dfte
#define DISTRHO_PLUGIN_UNIQUE_ID Strl

#define DISTRHO_PLUGIN_HAS_UI          1
#define DISTRHO_PLUGIN_IS_RT_SAFE      1
// DPF counts channels, not buses: one stereo pair per side
#define DISTRHO_PLUGIN_NUM_INPUTS      2
#define DISTRHO_PLUGIN_NUM_OUTPUTS     2
#define DISTRHO_PLUGIN_WANT_MIDI_INPUT  1
#define DISTRHO_PLUGIN_WANT_MIDI_OUTPUT 1
#define DISTRHO_PLUGIN_WANT_PROGRAMS   1
// static modgui shipped in modgui/ (rendered from Artwork.hpp), not DPF's
// generated one; this only adds the manifest reference to modgui.ttl
#define DISTRHO_PLUGIN_USES_MODGUI        1
#define DISTRHO_PLUGIN_USES_CUSTOM_MODGUI 1

#define DISTRHO_PLUGIN_LV2_CATEGORY    "lv2:InstrumentPlugin"
#define DISTRHO_PLUGIN_VST3_CATEGORIES "Instrument|MIDI"
#define DISTRHO_PLUGIN_CLAP_FEATURES   "instrument"

#define DISTRHO_UI_USE_CAIRO       1
#define DISTRHO_UI_USER_RESIZABLE  1
#define DISTRHO_UI_DEFAULT_WIDTH   480
#define DISTRHO_UI_DEFAULT_HEIGHT  660

// Parameter order matches the MIDI engine in PluginStringler.cpp;
// the passthrough DSP has no controls of its own. Bypass is ours.
enum Parameters {
    kParamMode,       // 0 mono tracking, 1 polyphonic
    kParamBend,       // master bend range in semitones, 0..48
    kParamSteal,      // note-steal range in semitones, 0..12
    kParamGate,       // velocity gate, 0..1
    kParamLegato,     // 0 off, 1 on
    kParamLegatoMs,   // glide time in ms, 0..100
    kParamVibDepth,   // vibrato depth in cents, 0..200
    kParamVibRate,    // vibrato rate in Hz, 0..10
    kParamChordShift, // chromatic shift in semitones, -12..12
    kParamChordWheel, // 0 off, 1 major, 2 minor, 3 diminished
    kParamTranspose,  // semitones, -24..24
    kParamScale,      // 0 off, 1..17 = the seventeen scales
    kParamRoot,       // scale root, 0=C .. 11=B
    kParamOctDown,    // octave-down velocity multiplier, 0..2 (0 = off)
    kParamSplit,      // lows/highs split pitch, 0..64 (0 = off)
    kParamLowsChan,   // lows output channel, 1..16
    kParamHighsChan,  // highs output channel, 1..16
    kParamBrightAmt,  // brightness CC amount, 0..2
    kParamBrightCc,   // brightness CC number, 0..127
    kParamLoudAmt,    // loudness CC amount, 0..2
    kParamLoudCc,     // loudness CC number, 0..127
    kParamVelNorm,    // velocity normalize target, 0..127 (0 = off)
    kParamOutChan,    // output MIDI channel, 1..16
    kParamSource,     // 0 audio tracker, 1 MIDI in, 2 both
    kParamTrackLow,   // lowest tracked note (MIDI note), 33..60
    kParamTrackRes,   // pitch resolution 0..3 (more window, more latency)
    kParamTrackBright,// brightness compensation 0..4 (2 = neutral)
    kParamTrackJitter,// stability/jitter removal 0..3
    kParamBendTrig,   // bend vs trigger balance, 0..100 %
    kParamBypass,
    kParamCount
};

#endif // DISTRHO_PLUGIN_INFO_H_INCLUDED
