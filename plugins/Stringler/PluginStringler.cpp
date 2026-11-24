/*
 * Stringler - guitar MIDI: turns MIDI notes into expressive guitar MIDI.
 * DPF plugin wrapper. The audio path is a transparent stereo passthrough;
 * all instrument logic is MIDI-side:
 *
 *   note on -> gate (velocity) -> pitch transforms (chord shift,
 *            transpose) -> scale filter -> velocity normalize -> voice
 *            allocation (mono takeover / polyphonic note stealing)
 *            -> legato glide (pitch-bend ramp, deferred ms) ->
 *            octave-down companion -> chord wheel -> CC dynamics
 *            (brightness / loudness from velocity) -> lows/highs split
 *            -> output channel
 *
 * Behaviour constants were recovered from the shipped binary of the
 * original commercial guitar-MIDI plugin (see README, "Research"):
 *   - master bend range 0..48 semitones ("master bend range", presets
 *     2/4/12/48; 0 = "Don't bend notes, for pianos/organs")
 *   - note stealing: "Steal notes within 1..12 semitones"
 *   - gate: "Notes will trigger when the indicator is above the gate."
 *   - legato: "Make notes connect into legato transitions", "Trade
 *     legato for longer notes" (DeferredMs)
 *   - auto vibrato: sin(duration/1000 * freq * 2pi) * cents / 100,
 *     freq 0..10 Hz, cents 0..200 (MidiMachine example script)
 *   - chord wheel: "Pitch shift among three chords" - major {0,4,7},
 *     minor {0,3,7}, diminished {0,3,8} (Helpers/Chords.lua)
 *   - scale filter: "Only pass through pitches in the scale", 17 scales
 *     with exact interval vectors (Helpers/Scales.lua)
 *   - brightness: cc default 1, amount 0..2, "brightness=min(1,
 *     n.brightness*amount)"; loudness: cc default 7, amount 0..2
 *   - split: split_pitch 40..64 default 50, lows_channel/highs_channel
 *   - octave-down: "Also play a note an octave lower with half the
 *     velocity", velocity_multiplier 0..2 default 0.5
 *   - velocity normalize: target velocity 1..127
 *
 * Estimates (flagged per house rule 4): the glide ramp shape (linear),
 * the CC-from-velocity proxy (the original measures picking dynamics
 * on audio input), the chord-wheel companion velocities, and the
 * velocity-normalize law (scaling towards the target). The inverse CC
 * option and the per-string/HEX features were not reproduced.
 *
 * Copyright (C) 2026 Dataforte
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "DistrhoPlugin.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

// Preset table and parameter defaults, generated from presets/stringler.json
#include "Presets.hpp"
#include "ParamNames.hpp"

START_NAMESPACE_DISTRHO

// --------------------------------------------------------------------
// The seventeen scales of the original (Helpers/Scales.lua), verbatim.
// Interval vectors in semitones, terminated by -1.
// --------------------------------------------------------------------
static constexpr int kScaleVecs[17][13] = {
    { 0, 2, 4, 5, 7, 9, 11, 12, -1, 0, 0, 0, 0 }, // major
    { 0, 2, 3, 5, 7, 8, 10, 12, -1, 0, 0, 0, 0 }, // minor
    { 0, 2, 3, 5, 7, 8, 11, 12, -1, 0, 0, 0, 0 }, // harmonic minor
    { 0, 2, 3, 5, 7, 9, 11, 12, -1, 0, 0, 0, 0 }, // melodic minor
    { 0, 2, 3, 5, 7, 9, 10, 12, -1, 0, 0, 0, 0 }, // dorian
    { 0, 2, 3, 5, 6, 8, 10, 12, -1, 0, 0, 0, 0 }, // locrian
    { 0, 2, 3, 5, 6, 8, 9, 11, 12, -1, 0, 0, 0 }, // diminished
    { 0, 3, 4, 7, 8, 11, 12, -1, 0, 0, 0, 0, 0 }, // augmented
    { 0, 1, 3, 4, 5, 6, 8, 10, 12, -1, 0, 0, 0 }, // spanish
    { 0, 1, 3, 4, 5, 7, 8, 10, 12, -1, 0, 0, 0 }, // flamenco
    { 0, 2, 4, 5, 7, 9, 10, 12, -1, 0, 0, 0, 0 }, // mixolydian
    { 0, 2, 4, 6, 7, 9, 11, 12, -1, 0, 0, 0, 0 }, // lydian
    { 0, 2, 4, 6, 7, 8, 10, 12, -1, 0, 0, 0, 0 }, // lydian minor
    { 0, 2, 4, 6, 8, 9, 10, 12, -1, 0, 0, 0, 0 }, // lydian augmented
    { 0, 2, 4, 6, 7, 9, 10, 12, -1, 0, 0, 0, 0 }, // lydian dominant
    { 0, 3, 5, 6, 7, 10, 12, -1, 0, 0, 0, 0, 0 }, // blues
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 }, // chromatic
};

static uint32_t scaleMask(const int* v)
{
    uint32_t m = 0;
    for (int i = 0; v[i] >= 0; ++i)
        m |= (1u << v[i]);
    return m;
}

// membership masks for the scale filter (index 0 unused)
static const uint32_t kScaleMasks[18] = {
    0,
    scaleMask(kScaleVecs[0]),  scaleMask(kScaleVecs[1]),  scaleMask(kScaleVecs[2]),
    scaleMask(kScaleVecs[3]),  scaleMask(kScaleVecs[4]),  scaleMask(kScaleVecs[5]),
    scaleMask(kScaleVecs[6]),  scaleMask(kScaleVecs[7]),  scaleMask(kScaleVecs[8]),
    scaleMask(kScaleVecs[9]),  scaleMask(kScaleVecs[10]), scaleMask(kScaleVecs[11]),
    scaleMask(kScaleVecs[12]), scaleMask(kScaleVecs[13]), scaleMask(kScaleVecs[14]),
    scaleMask(kScaleVecs[15]), scaleMask(kScaleVecs[16]),
};

// chord-wheel intervals (Helpers/Chords.lua, verbatim); the wheel cycles
// major -> minor -> diminished on every fresh note ("pitch shift among
// three chords")
static constexpr int kChordVecs[3][4] = {
    { 0, 4, 7, -1 }, // major
    { 0, 3, 7, -1 }, // minor
    { 0, 3, 8, -1 }, // diminished
};

// --------------------------------------------------------------------
// Raw MIDI event codec (DPF passes raw MidiEvent structs)
// --------------------------------------------------------------------
namespace {

enum EvType { kOther = 0, kNoteOn, kNoteOff, kControl, kPitchBend };

struct MidEv {
    uint8_t type = kOther;
    uint8_t ch = 0;    // 0-based
    uint8_t d1 = 0;
    uint8_t d2 = 0;
    uint32_t frame = 0;
};

MidEv decodeMidi(const MidiEvent& e)
{
    MidEv ev;
    ev.frame = e.frame;
    if (e.size < 1)
        return ev;
    const uint8_t st = e.data[0];
    // MIDI status byte: command in the high nibble, channel in the low one
    ev.ch = st & 0x0F;
    switch (st & 0xF0) {
    case 0x90:
        ev.type = kNoteOn;
        if (e.size >= 3) { ev.d1 = e.data[1]; ev.d2 = e.data[2]; }
        break;
    case 0x80:
        ev.type = kNoteOff;
        if (e.size >= 3) { ev.d1 = e.data[1]; ev.d2 = e.data[2]; }
        break;
    case 0xB0:
        ev.type = kControl;
        ev.d1 = (e.size >= 2) ? e.data[1] : 0;
        ev.d2 = (e.size >= 3) ? e.data[2] : 0;
        break;
    case 0xE0:
        ev.type = kPitchBend;
        ev.d1 = (e.size >= 2) ? e.data[1] : 0;
        ev.d2 = (e.size >= 3) ? e.data[2] : 0;
        break;
    default:
        break;
    }
    return ev;
}

MidiEvent encodeNote(uint8_t ch, int pitch, int vel, uint32_t frame)
{
    MidiEvent e;
    e.frame = frame;
    e.size = 3;
    e.dataExt = nullptr;
    e.data[0] = static_cast<uint8_t>((vel ? 0x90 : 0x80) | (ch & 0x0F));
    e.data[1] = static_cast<uint8_t>(pitch & 0x7F);
    e.data[2] = static_cast<uint8_t>(vel & 0x7F);
    return e;
}

MidiEvent encodeControl(uint8_t ch, int cc, int val, uint32_t frame)
{
    MidiEvent e;
    e.frame = frame;
    e.size = 3;
    e.dataExt = nullptr;
    e.data[0] = static_cast<uint8_t>(0xB0 | (ch & 0x0F));
    e.data[1] = static_cast<uint8_t>(cc & 0x7F);
    e.data[2] = static_cast<uint8_t>(val & 0x7F);
    return e;
}

MidiEvent encodePitchBend(uint8_t ch, int val, uint32_t frame)
{
    MidiEvent e;
    e.frame = frame;
    e.size = 3;
    e.dataExt = nullptr;
    e.data[0] = static_cast<uint8_t>(0xE0 | (ch & 0x0F));
    e.data[1] = static_cast<uint8_t>(val & 0x7F);
    e.data[2] = static_cast<uint8_t>((val >> 7) & 0x7F);
    return e;
}

} // namespace

// --------------------------------------------------------------------
// Voice: one sounded note, plus octave-down and chord-wheel companions.
// --------------------------------------------------------------------
struct Voice
{
    void reset()
    {
        active = false;
        ch = 0;
        inCh = 0;
        pitch = 0;
        rawPitch = 0;
        vel = 0;
        chord1 = 0;
        chord2 = 0;
        oct1 = 0;
        gliding = false;
        fromCh = 0;
        fromPitch = 0;
        toPitch = 0;
        glideStart = 0.0;
        glideDur = 0.0;
        pendChord1 = 0;
        pendChord2 = 0;
        pendOct = 0;
        vibPhase = 0.0;
        bendOffset = 0.0;
        lastBend = 8192;
        lastBendFrame = 0.0;
    }
    bool   active = false;
    uint8_t ch    = 0;    // output (wire) channel of the note
    uint8_t inCh  = 0;    // input channel, used to match note offs
    int      pitch  = 0;  // transformed pitch of the root (currently sounding)
    int      rawPitch = 0;// incoming (pre-transform) pitch; note offs match on it
    int      vel    = 0;
    int      chord1 = 0;  // chord-wheel companions (0 = none)
    int      chord2 = 0;
    int      oct1   = 0;  // octave-down companion (0 = none)
    bool      gliding = false;
    uint8_t   fromCh = 0;      // glide: channel of the old note (its note off goes there)
    int       fromPitch = 0;   // glide: old pitch
    int       toPitch   = 0;   // glide: new pitch
    double    glideStart = 0.0;// absolute frame counter
    double    glideDur   = 0.0;// frames
    int       pendChord1 = 0;  // chord companions to emit when the glide lands
    int       pendChord2 = 0;
    int       pendOct    = 0;  // octave companion to emit when the glide lands
    double    vibPhase = 0.0;  // rad
    double    bendOffset = 0.0;// cents->semitones from the audio tracker
    int       lastBend = 8192; // last 14-bit pitch-bend sent
    double    lastBendFrame = 0.0;
};

// --------------------------------------------------------------------
// Audio tracker (monophonic)
//
// FFT-accelerated YIN: the squared-difference function comes from the
// autocorrelation (d(tau) = m0 + m(tau) - 2*acf(tau)); the cumulative
// mean normalized difference picks the first period below threshold and
// a parabolic fit refines it. The analysis window is sized from the
// lowest accepted note (~70 Hz) - which is why the original's latency
// grows for low notes. A note is committed after two consecutive
// agreeing hops (stability), velocity comes from the RMS level, and the
// existing GATE parameter therefore acts as the input-level threshold
// (level mapping: -60 dB .. 0 dB -> velocity 0..127).
// --------------------------------------------------------------------
struct AudioTracker
{
    static constexpr int kMaxNotes = 4;   // poly voices
    static constexpr int kMaxVoices = 8;  // tracked + pending
    static constexpr int kHarmonics = 8;  // comb length

    struct PVoice { int note; int stable; int miss; bool on; };

    double sr = 48000.0;
    int window = 1408, hop = 256, lagMin = 40, lagMax = 686, fftN = 4096;
    int ring = 4096;                    // allocated history (>= any window)
    int lagMaxAlloc = 686, windowAlloc = 4096;
    int fftAlloc = 4096, nfft = 4096;   // FFT size grows with the window
    int lowNote = 40;                   // lowest tracked note (MIDI)
    int stableNeed = 2;                 // agreeing analyses before commit
    float brightTilt = 1.0f;            // poly harmonic tilt
    float bendWindow = 1.5f;            // semitones treated as a bend
    int window2 = 4096, fft2N = 8192;   // poly: longer window, finer grid
    std::vector<float> hist, re, im, cosTab, sinTab, hann;
    std::vector<float> mag, wmag, dif, cm, sq;
    std::vector<float> hist2, re2, im2, cosTab2, sinTab2, hann2, mag2, wmag2, env2;
    int histFill = 0, hopFill = 0, hopTick = 0;
    int hist2Fill = 0, hopTick2 = 0;
    int hist2Count = 0;   // samples seen, capped at window2
    bool sounding = false;
    int tracked = 0, cand = -1, stable = 0, silence = 0;
    PVoice pv[kMaxVoices];
    int pvCount = 0;

    void configure(double sampleRate)
    {
        sr = sampleRate < 8000.0 ? 48000.0 : sampleRate;
        lagMin = std::max(2, static_cast<int>(sr / 1200.0));
        // allocate for the widest setting: lowest note A1 (55 Hz) at the
        // highest pitch resolution (3.5 periods)
        lagMaxAlloc = std::max(lagMin + 8, static_cast<int>(std::ceil(sr / 55.0)));
        windowAlloc = (static_cast<int>(4.5 * lagMaxAlloc) + 63) / 64 * 64;
        hop = std::max(16, static_cast<int>(sr / 187.5 + 0.5));
        fftAlloc = 1;
        while (fftAlloc < windowAlloc + lagMaxAlloc)
            fftAlloc <<= 1;
        nfft = fftAlloc;
        fftN = fftAlloc;
        ring = windowAlloc;
        hist.assign(ring, 0.0f);
        re.assign(fftAlloc, 0.0f);
        im.assign(fftAlloc, 0.0f);
        dif.assign(lagMaxAlloc + 2, 0.0f);
        cm.assign(lagMaxAlloc + 2, 0.0f);
        sq.assign(ring + 1, 0.0f);
        mag.assign(fftAlloc / 2 + 1, 0.0f);
        wmag.assign(fftAlloc / 2 + 1, 0.0f);
        hann.assign(ring, 0.0f);
        window = 0;
        lagMax = 0;
        setRange(40, 0);
        // one table for every power-of-two size below fftAlloc: the table
        // index for a given transform length is independent of the size
        cosTab.resize(fftAlloc / 2);
        sinTab.resize(fftAlloc / 2);
        for (int i = 0; i < fftAlloc / 2; ++i) {
            const double a = 6.283185307179586 * i / fftAlloc;
            cosTab[i] = static_cast<float>(std::cos(a));
            sinTab[i] = static_cast<float>(std::sin(a));
        }
        // poly path: ~85 ms window and a 2x zero-padded grid
        window2 = std::max(windowAlloc, static_cast<int>(sr * 0.0852 + 0.5));
        fft2N = 1;
        while (fft2N < 2 * window2)
            fft2N <<= 1;
        hist2.assign(window2, 0.0f);
        re2.assign(fft2N, 0.0f);
        im2.assign(fft2N, 0.0f);
        mag2.assign(fft2N / 2 + 1, 0.0f);
        wmag2.assign(fft2N / 2 + 1, 0.0f);
        env2.assign(fft2N / 2 + 1, 0.0f);
        hann2.resize(window2);
        for (int i = 0; i < window2; ++i)
            hann2[i] = static_cast<float>(0.5 * (1.0 - std::cos(6.283185307179586 * i / (window2 - 1))));
        cosTab2.resize(fft2N / 2);
        sinTab2.resize(fft2N / 2);
        for (int i = 0; i < fft2N / 2; ++i) {
            const double a = 6.283185307179586 * i / fft2N;
            cosTab2[i] = static_cast<float>(std::cos(a));
            sinTab2[i] = static_cast<float>(std::sin(a));
        }
        reset();
    }

    // lowest note and pitch resolution choose the analysis window: more
    // periods => better low-note resolution but more latency (the original's
    // "higher resolution => more latency", "lower notes => more latency")
    void setRange(int lowest, int resolution)
    {
        lowNote = std::max(21, std::min(108, lowest));
        const float f0 = 440.0f * static_cast<float>(std::pow(2.0, (lowNote - 69) / 12.0));
        int lm = static_cast<int>(std::ceil(sr / std::max(30.0f, f0)));
        lm = std::max(lagMin + 8, std::min(lagMaxAlloc, lm));
        const int res = std::max(0, std::min(3, resolution));
        const float periods = 3.0f + 0.5f * res;
        int w = static_cast<int>(periods * lm + 0.5f) + 32;
        w = std::min(windowAlloc, w);
        w = std::max(2 * lm + 32, w);
        if (w != window) {
            window = w;
            for (int i = 0; i < window; ++i)
                hann[i] = static_cast<float>(0.5 * (1.0 - std::cos(6.283185307179586 * i / (window - 1))));
        }
        lagMax = lm;
        nfft = 1;
        while (nfft < window + lagMax)
            nfft <<= 1;
        if (nfft > fftAlloc)
            nfft = fftAlloc;
    }

    int histIndex(int i) const
    {
        int idx = histFill - window + i;
        if (idx < 0)
            idx += ring;
        return idx;
    }

    void reset()
    {
        std::fill(hist.begin(), hist.end(), 0.0f);
        std::fill(hist2.begin(), hist2.end(), 0.0f);
        histFill = hopFill = hopTick = 0;
        hist2Fill = hopTick2 = 0;
        hist2Count = 0;
        sounding = false;
        tracked = 0;
        cand = -1;
        stable = 0;
        silence = 0;
        pvCount = 0;
    }

    /* release every note the tracker currently holds */
    template <typename NoteOff>
    void flush(const NoteOff& offNote)
    {
        if (sounding) {
            offNote(tracked, 0);
            sounding = false;
        }
        for (int v = 0; v < pvCount; ++v)
            if (pv[v].on)
                offNote(pv[v].note, 0);
        pvCount = 0;
        cand = -1;
        stable = 0;
    }

    void fft(bool inverse)
    {
        const int n = nfft;
        for (int i = 1, j = 0; i < n; ++i) {
            int bit = n >> 1;
            for (; j & bit; bit >>= 1)
                j ^= bit;
            j ^= bit;
            if (i < j) {
                std::swap(re[i], re[j]);
                std::swap(im[i], im[j]);
            }
        }
        for (int len = 2; len <= n; len <<= 1) {
            const int half = len >> 1, step = fftAlloc / len;
            for (int i = 0; i < n; i += len) {
                for (int j = 0; j < half; ++j) {
                    const int k = j * step;
                    const float wr = cosTab[k];
                    const float wi = (inverse ? sinTab[k] : -sinTab[k]);
                    const float ur = re[i + j], ui = im[i + j];
                    const float vr = re[i + j + half] * wr - im[i + j + half] * wi;
                    const float vi = re[i + j + half] * wi + im[i + j + half] * wr;
                    re[i + j] = ur + vr;
                    im[i + j] = ui + vi;
                    re[i + j + half] = ur - vr;
                    im[i + j + half] = ui - vi;
                }
            }
        }
        if (inverse) {
            const float s = 1.0f / n;
            for (int i = 0; i < n; ++i) {
                re[i] *= s;
                im[i] *= s;
            }
        }
    }

    void fft2(bool inverse)
    {
        for (int i = 1, j = 0; i < fft2N; ++i) {
            int bit = fft2N >> 1;
            for (; j & bit; bit >>= 1)
                j ^= bit;
            j ^= bit;
            if (i < j) {
                std::swap(re2[i], re2[j]);
                std::swap(im2[i], im2[j]);
            }
        }
        for (int len = 2; len <= fft2N; len <<= 1) {
            const int half = len >> 1, step = fft2N / len;
            for (int i = 0; i < fft2N; i += len) {
                for (int j = 0; j < half; ++j) {
                    const int k = j * step;
                    const float wr = cosTab2[k];
                    const float wi = (inverse ? sinTab2[k] : -sinTab2[k]);
                    const float ur = re2[i + j], ui = im2[i + j];
                    const float vr = re2[i + j + half] * wr - im2[i + j + half] * wi;
                    const float vi = re2[i + j + half] * wi + im2[i + j + half] * wr;
                    re2[i + j] = ur + vr;
                    im2[i + j] = ui + vi;
                    re2[i + j + half] = ur - vr;
                    im2[i + j + half] = ui - vi;
                }
            }
        }
        if (inverse) {
            const float s = 1.0f / fft2N;
            for (int i = 0; i < fft2N; ++i) {
                re2[i] *= s;
                im2[i] *= s;
            }
        }
    }

    static float noteFreq(int note)
    {
        return 440.0f * static_cast<float>(std::pow(2.0, (note - 69) / 12.0));
    }

    template <typename NoteOn, typename Bend, typename NoteOff>
    void processAudio(const float* l, const float* r, const uint32_t frames,
                      const double blockStart, const float gateVel, const bool poly,
                      NoteOn onNote, Bend onBend, NoteOff offNote)
    {
        for (uint32_t i = 0; i < frames; ++i) {
            const float x = 0.5f * (l[i] + r[i]);
            hist[histFill] = x;
            if (++histFill == ring)
                histFill = 0;
            hist2[hist2Fill] = x;
            if (++hist2Fill == window2)
                hist2Fill = 0;
            if (hist2Count < window2)
                ++hist2Count;
            if (++hopFill >= hop) {
                hopFill = 0;
                // poly runs on every 4th hop over its longer window (more
                // polyphony, more latency - the original's trade-off too)
                if (poly && (hopTick++ & 3) != 0)
                    continue;
                analyse(static_cast<uint32_t>(i), blockStart, gateVel, poly,
                        onNote, onBend, offNote);
            }
        }
    }

    template <typename NoteOn, typename Bend, typename NoteOff>
    void analyse(const uint32_t frame, const double blockStart, const float gateVel,
                 const bool poly, NoteOn& onNote, Bend& onBend, NoteOff& offNote)
    {
        double energy = 0.0;
        for (int i = 0; i < window; ++i) {
            const int idx = histFill + i;
            const float x = hist[idx >= window ? idx - window : idx];
            energy += static_cast<double>(x) * x;
        }
        const float rms = static_cast<float>(std::sqrt(energy / window));
        const float db = 20.0f * std::log10(rms + 1e-9f);
        int vel = static_cast<int>(std::lround(127.0f * (db + 60.0f) / 60.0f));
        if (vel < 0) vel = 0;
        if (vel > 127) vel = 127;
        const bool above = (db > -70.0f) && (vel >= static_cast<int>(gateVel));

        if (poly) {
            if (above && hist2Count >= window2)
                analysePoly(frame, blockStart, vel, onNote, onBend, offNote);
            else
                polySilence(frame, offNote);
            return;
        }
        if (above)
            analyseMono(frame, blockStart, vel, onNote, onBend, offNote);
        else
            monoSilence(frame, offNote);
    }

    template <typename NoteOff>
    void monoSilence(const uint32_t frame, NoteOff& offNote)
    {
        if (++silence >= 2 && sounding) {
            offNote(tracked, frame);
            sounding = false;
        }
        cand = -1;
        stable = 0;
    }

    template <typename NoteOff>
    void polySilence(const uint32_t frame, NoteOff& offNote)
    {
        for (int v = 0; v < pvCount; ) {
            if (++pv[v].miss >= 2) {
                if (pv[v].on)
                    offNote(pv[v].note, frame);
                pv[v] = pv[--pvCount];
            } else {
                ++v;
            }
        }
    }

    template <typename NoteOn, typename Bend, typename NoteOff>
    void analyseMono(const uint32_t frame, const double blockStart, const int vel,
                     NoteOn& onNote, Bend& onBend, NoteOff& offNote)
    {
        (void)offNote;
        silence = 0;
        for (int i = 0; i < window; ++i) {
            re[i] = hist[histIndex(i)];
            im[i] = 0.0f;
        }
        for (int i = window; i < nfft; ++i) {
            re[i] = 0.0f;
            im[i] = 0.0f;
        }
        fft(false);
        for (int i = 0; i < nfft; ++i) {
            re[i] = re[i] * re[i] + im[i] * im[i];
            im[i] = 0.0f;
        }
        fft(true);

        sq[window] = 0.0f;
        for (int i = window - 1; i >= 0; --i) {
            const float x = hist[histIndex(i)];
            sq[i] = sq[i + 1] + x * x;
        }
        const float m0 = sq[0];
        for (int tau = 1; tau <= lagMax; ++tau)
            dif[tau] = std::max(0.0f, m0 + sq[tau] - 2.0f * re[tau]);

        float running = 0.0f, best = 1.0e9f;
        cm[0] = 1.0f;
        int tauBest = -1;
        for (int tau = 1; tau <= lagMax; ++tau) {
            running += dif[tau];
            cm[tau] = running > 0.0f ? dif[tau] * tau / running : 1.0f;
            if (tau >= lagMin && cm[tau] < best) {
                best = cm[tau];
                tauBest = tau;
            }
        }
        if (best < 0.25f) {
            for (int tau = lagMin; tau < lagMax; ++tau) {
                if (cm[tau] < 0.12f) {
                    while (tau + 1 <= lagMax && cm[tau + 1] < cm[tau])
                        ++tau;
                    tauBest = tau;
                    break;
                }
            }
        } else {
            tauBest = -1;
        }
        if (tauBest < 0)
            return;

        float tau = static_cast<float>(tauBest);
        if (tauBest > 1 && tauBest < lagMax) {
            const float a = cm[tauBest - 1], b = cm[tauBest], c = cm[tauBest + 1];
            const float den = a - 2.0f * b + c;
            if (std::fabs(den) > 1e-9f) {
                float shift = 0.5f * (a - c) / den;
                if (shift < -0.5f) shift = -0.5f;
                if (shift > 0.5f) shift = 0.5f;
                tau += shift;
            }
        }
        const float f0 = static_cast<float>(sr) / tau;
        if (f0 < noteFreq(lowNote) * 0.94f || f0 > 1500.0f)
            return;
        const float midi = 69.0f + 12.0f * std::log2(f0 / 440.0f);
        const int note = static_cast<int>(std::lround(midi));
        if (note < 24 || note > 100)
            return;
        const float cents = (midi - static_cast<float>(note)) * 100.0f;

        // a sounding note that drifts by up to the bend window is a bend
        // (the original's "note bend vs triggers" behaviour)
        if (sounding) {
            const float delta = midi - static_cast<float>(tracked);
            if (std::fabs(delta) <= bendWindow) {
                onBend(tracked, delta * 100.0f, frame);
                cand = tracked;
                stable = stableNeed;
                return;
            }
        }

        if (note != cand) {
            cand = note;
            stable = 1;
        } else if (stable < stableNeed + 1) {
            ++stable;
        }
        if (stable < stableNeed)
            return;
        if (sounding && note == tracked)
            return;

        // mono: the note-on takes over (the engine glides or retriggers)
        onNote(note, std::max(vel, 1), frame, blockStart);
        onBend(note, cents, frame);
        tracked = note;
        sounding = true;
    }

    // Harmonic-comb multi-pitch: score every guitar note over harmonics
    // 1..8 (odd harmonics weighted higher), take up to 4 peaks, and cancel
    // each winner's harmonics before the next pick (suppresses octave
    // errors). Voices need two agreeing analyses before a note is emitted
    // and are released after two misses.
    template <typename NoteOn, typename Bend, typename NoteOff>
    void analysePoly(const uint32_t frame, const double blockStart, const int vel,
                     NoteOn& onNote, Bend& onBend, NoteOff& offNote)
    {
        (void)onBend;
        for (int i = 0; i < window2; ++i) {
            const int idx = hist2Fill + i;
            re2[i] = hist2[idx >= window2 ? idx - window2 : idx] * hann2[i];
            im2[i] = 0.0f;
        }
        for (int i = window2; i < fft2N; ++i) {
            re2[i] = 0.0f;
            im2[i] = 0.0f;
        }
        fft2(false);
        const int bins = fft2N / 2;
        float tiltW[kHarmonics + 1];
        tiltW[1] = 1.0f;
        for (int h = 2; h <= kHarmonics; ++h)
            tiltW[h] = tiltW[h - 1] * brightTilt;
        // sqrt-magnitude compression: keeps peaks far above the numerical
        // noise floor (empty bins would otherwise whiten just as strongly)
        for (int i = 0; i <= bins; ++i) {
            mag2[i] = std::sqrt(re2[i] * re2[i] + im2[i] * im2[i]);
            wmag2[i] = std::sqrt(mag2[i]);
        }

        // interpolated spectral peaks (sqrt magnitude)
        struct Peak { float f, m; };
        Peak peaks[96];
        int np = 0;
        float maxW = 0.0f;
        for (int b = 2; b < bins; ++b)
            if (wmag2[b] > maxW)
                maxW = wmag2[b];
        for (int b = 2; b < bins - 1 && np < 96; ++b) {
            if (wmag2[b] <= wmag2[b - 1] || wmag2[b] < wmag2[b + 1])
                continue;
            if (b >= 2 && wmag2[b] < wmag2[b - 2])
                continue;
            if (b + 2 < bins && wmag2[b] < wmag2[b + 2])
                continue;
            if (wmag2[b] < 0.20f * maxW)   // exclude window sidelobes
                continue;
            const float a = wmag2[b - 1], c = wmag2[b], d = wmag2[b + 1];
            const float den = a - 2.0f * c + d;
            float delta = 0.0f;
            if (std::fabs(den) > 1e-9f) {
                delta = 0.5f * (a - d) / den;
                if (delta < -0.5f) delta = -0.5f;
                if (delta > 0.5f) delta = 0.5f;
            }
            peaks[np].f = (b + delta) * static_cast<float>(sr) / fft2N;
            peaks[np].m = c;
            ++np;
        }
        bool used[96] = {};

        int det[kMaxNotes];
        int nd = 0;
        float first = 0.0f;
        for (int k = 0; k < kMaxNotes; ++k) {
            int bestNote = -1;
            float bestScore = 0.0f;
            for (int note = lowNote; note <= 88; ++note) {
                const float f0 = noteFreq(note);
                float acc = 0.0f;
                for (int h = 1; h <= kHarmonics; ++h) {
                    const float f = f0 * h;
                    if (f > static_cast<float>(sr) * 0.45f)
                        break;
                    // closest unused peak within 60 cents
                    int bi = -1;
                    float bd = 60.0f;   // cents
                    for (int p = 0; p < np; ++p) {
                        if (used[p])
                            continue;
                        const float cents = std::fabs(1200.0f * std::log2(peaks[p].f / f));
                        if (cents < bd) {
                            bd = cents;
                            bi = p;
                        }
                    }
                    if (bi < 0)
                        continue;
                    const float w = (h == 1) ? 1.0f : ((h & 1) ? 0.5f : 0.3f);
                    acc += w * peaks[bi].m * tiltW[h];
                }
                if (acc > bestScore) {
                    bestScore = acc;
                    bestNote = note;
                }
            }
            if (k == 0) {
                first = bestScore;
                if (first < 1.0f)   // nothing pitched enough
                    break;
            }
            if (bestNote < 0 || bestScore <= 0.0f || bestScore < 0.25f * first)
                break;
            det[nd++] = bestNote;
            // retire the peaks this note explains (octave-error suppression)
            const float f0 = noteFreq(bestNote);
            for (int h = 1; h <= kHarmonics; ++h) {
                const float f = f0 * h;
                if (f > static_cast<float>(sr) * 0.45f)
                    break;
                for (int p = 0; p < np; ++p) {
                    if (used[p])
                        continue;
                    if (std::fabs(1200.0f * std::log2(peaks[p].f / f)) < 60.0f)
                        used[p] = true;
                }
            }
        }


        // release voices that were not detected twice in a row
        for (int v = 0; v < pvCount; ) {
            bool hit = false;
            for (int i = 0; i < nd; ++i)
                if (det[i] == pv[v].note)
                    hit = true;
            if (hit) {
                pv[v].miss = 0;
            } else if (++pv[v].miss >= 2) {
                if (pv[v].on)
                    offNote(pv[v].note, frame);
                pv[v] = pv[--pvCount];
                continue;
            }
            ++v;
        }

        // commit new notes after two agreeing analyses
        for (int i = 0; i < nd; ++i) {
            int v = -1;
            for (int j = 0; j < pvCount; ++j)
                if (pv[j].note == det[i]) {
                    v = j;
                    break;
                }
            if (v < 0) {
                if (pvCount < kMaxVoices)
                    pv[pvCount++] = PVoice{ det[i], 1, 0, false };
            } else if (!pv[v].on) {
                if (++pv[v].stable >= stableNeed) {
                    onNote(pv[v].note, std::max(vel, 1), frame, blockStart);
                    pv[v].on = true;
                }
            }
        }
    }
};

// Named values for the discrete parameters, built once from ParamNames.hpp;
// hosts show these as dropdowns and the faceplate uses selector badges.
static ParameterEnumerationValue* makeEnum(const sl_names::Item* items, const int count)
{
    auto* const values = new ParameterEnumerationValue[count];
    for (int i = 0; i < count; ++i)
        values[i] = ParameterEnumerationValue(static_cast<float>(items[i].value),
                                              items[i].label);
    return values;
}

static ParameterEnumerationValue* const kModeValues = makeEnum(sl_names::kMode, sl_names::kModeCount);
static ParameterEnumerationValue* const kLegatoValues = makeEnum(sl_names::kLegato, sl_names::kLegatoCount);
static ParameterEnumerationValue* const kSourceValues = makeEnum(sl_names::kSource, sl_names::kSourceCount);
static ParameterEnumerationValue* const kWheelValues = makeEnum(sl_names::kWheel, sl_names::kWheelCount);
static ParameterEnumerationValue* const kRootValues = makeEnum(sl_names::kRoot, sl_names::kRootCount);
static ParameterEnumerationValue* const kScaleValues = makeEnum(sl_names::kScale, sl_names::kScaleCount);
static ParameterEnumerationValue* const kChannelValues = makeEnum(sl_names::kChannel, sl_names::kChannelCount);
static ParameterEnumerationValue* const kSplitValues = makeEnum(sl_names::kSplit, sl_names::kSplitCount);

// ParameterEnumerationValues has no copy assignment (it owns its array),
// so fill the fields directly for a statically declared table.
static void setEnumValues(Parameter& p, uint8_t count, ParameterEnumerationValue* values)
{
    p.enumValues.count = count;
    p.enumValues.restrictedMode = true;
    p.enumValues.values = values;
    p.enumValues.deleteLater = false;
}

class PluginStringler : public Plugin
{
public:
    PluginStringler()
        : Plugin(kParamCount, preset_data::kCount, 0)
    {
        for (uint32_t i = 0; i < kParamCount; ++i)
            fParams[i] = (i == kParamBypass) ? 0.0f
                                             : preset_data::kDefaults.value[i];
        fFrame = 0.0;
        fWheelIdx = 0;
        fRand.seed(0xC0FFEE);
        fPrimary = nullptr;
        fLastPitch = 60;
        for (auto& v : fVoices)
            v.reset();
    }

protected:
    const char* getLabel() const override { return "stringler"; }
    const char* getDescription() const override
    {
        return "Guitar MIDI: turns MIDI notes into expressive guitar MIDI - "
               "legato glides, note stealing, automatic vibrato, chord "
               "shifting and wheel, scale filtering and velocity-driven "
               "brightness/loudness CC. Functional recreation of the core "
               "tracking/expression pipeline of a commercial guitar-MIDI "
               "plugin.";
    }
    const char* getMaker() const override { return "Dataforte"; }
    const char* getHomePage() const override { return "https://dataforte.net"; }
    const char* getLicense() const override { return "GPL-2.0-or-later"; }
    uint32_t getVersion() const override { return d_version(1, 0, 0); }

    void initAudioPort(bool input, uint32_t index, AudioPort& port) override
    {
        port.groupId = kPortGroupStereo;
        Plugin::initAudioPort(input, index, port);
        // unique symbols: hosts (Carla, mod-ui) map ports by symbol
        port.symbol = (index == 0) ? (input ? "in" : "out")
                                   : (input ? "in_r" : "out_r");
        port.name   = (index == 0) ? (input ? "In L" : "Out L")
                                   : (input ? "In R" : "Out R");
    }

    void initParameter(const uint32_t index, Parameter& p) override
    {
        p.hints = kParameterIsAutomatable;
        p.ranges.min = 0.0f;
        p.ranges.max = 1.0f;
        p.ranges.def = preset_data::kDefaults.value[index];

        switch (index)
        {
        case kParamMode:
            p.hints |= kParameterIsBoolean | kParameterIsInteger;
            p.name = "Tracking Mode"; p.shortName = "Track"; p.symbol = "mode";
            p.description = "0 = monophonic tracking, 1 = polyphonic";
            setEnumValues(p, 2, kModeValues);
            break;
        case kParamBend:
            p.hints |= kParameterIsInteger;
            p.name = "Bend Range"; p.shortName = "Bend"; p.symbol = "bend"; p.unit = "st";
            p.ranges.max = 48.0f;
            p.description = "Master bend range in semitones (0 = no bends)";
            break;
        case kParamSteal:
            p.hints |= kParameterIsInteger;
            p.name = "Note Steal"; p.shortName = "Steal"; p.symbol = "steal"; p.unit = "st";
            p.ranges.max = 12.0f;
            p.description = "Steal notes within this many semitones (0 = never)";
            break;
        case kParamGate:
            p.name = "Velocity Gate"; p.shortName = "Gate"; p.symbol = "gate";
            p.description = "Notes will trigger when the indicator is above the gate";
            break;
        case kParamLegato:
            p.hints |= kParameterIsBoolean | kParameterIsInteger;
            p.name = "Legato"; p.shortName = "Legato"; p.symbol = "legato";
            p.description = "Make notes connect into legato transitions";
            setEnumValues(p, 2, kLegatoValues);
            break;
        case kParamLegatoMs:
            p.name = "Glide Time"; p.shortName = "Glide"; p.symbol = "legatoms"; p.unit = "ms";
            p.ranges.max = 100.0f;
            p.description = "Legato glide time (trade legato for longer notes)";
            break;
        case kParamVibDepth:
            p.name = "Vibrato Depth"; p.shortName = "VibD"; p.symbol = "vibdepth"; p.unit = "cts";
            p.ranges.max = 200.0f;
            p.description = "Automatic vibrato depth in cents";
            break;
        case kParamVibRate:
            p.name = "Vibrato Rate"; p.shortName = "VibR"; p.symbol = "vibrate"; p.unit = "Hz";
            p.ranges.max = 10.0f;
            p.description = "Automatic vibrato rate";
            break;
        case kParamChordShift:
            p.hints |= kParameterIsInteger;
            p.name = "Chord Shift"; p.shortName = "Shift"; p.symbol = "chordshift"; p.unit = "st";
            p.ranges.min = -12.0f;
            p.ranges.max = 12.0f;
            p.description = "Chromatic pitch shift";
            break;
        case kParamChordWheel:
            p.hints |= kParameterIsInteger;
            p.name = "Chord Wheel"; p.shortName = "Wheel"; p.symbol = "chordwheel";
            p.ranges.max = 3.0f;
            p.description = "0 = off, 1 = major, 2 = minor, 3 = diminished (cycles)";
            setEnumValues(p, 4, kWheelValues);
            break;
        case kParamTranspose:
            p.hints |= kParameterIsInteger;
            p.name = "Transpose"; p.shortName = "Transp"; p.symbol = "transpose"; p.unit = "st";
            p.ranges.min = -24.0f;
            p.ranges.max = 24.0f;
            break;
        case kParamScale:
            p.hints |= kParameterIsInteger;
            p.name = "Scale Filter"; p.shortName = "Scale"; p.symbol = "scale";
            p.ranges.max = 17.0f;
            p.description = "0 = off, 1..17 = the seventeen scales";
            setEnumValues(p, 18, kScaleValues);
            break;
        case kParamRoot:
            p.hints |= kParameterIsInteger;
            p.name = "Root"; p.shortName = "Root"; p.symbol = "root";
            p.ranges.max = 11.0f;
            p.description = "Scale root (0 = C .. 11 = B)";
            setEnumValues(p, 12, kRootValues);
            break;
        case kParamOctDown:
            p.name = "Octave Down"; p.shortName = "OctD"; p.symbol = "octdown";
            p.ranges.max = 2.0f;
            p.description = "Also play a note an octave lower with this velocity multiplier (0 = off)";
            break;
        case kParamSplit:
            p.hints |= kParameterIsInteger;
            p.name = "Split Pitch"; p.shortName = "Split"; p.symbol = "split";
            setEnumValues(p, 26, kSplitValues);
            p.ranges.max = 64.0f;
            p.description = "Lows/highs split pitch (0 = off)";
            break;
        case kParamLowsChan:
            p.hints |= kParameterIsInteger;
            p.name = "Low Ch"; p.shortName = "LoCh"; p.symbol = "lowschan";
            setEnumValues(p, 16, kChannelValues);
            p.ranges.min = 1.0f;
            p.ranges.max = 16.0f;
            p.description = "Output channel for notes below the split pitch";
            break;
        case kParamHighsChan:
            p.hints |= kParameterIsInteger;
            p.name = "High Ch"; p.shortName = "HiCh"; p.symbol = "highschan";
            setEnumValues(p, 16, kChannelValues);
            p.ranges.min = 1.0f;
            p.ranges.max = 16.0f;
            p.description = "Output channel for notes at/above the split pitch";
            break;
        case kParamBrightAmt:
            p.name = "Brightness Amt"; p.shortName = "BrA"; p.symbol = "brightamt";
            p.ranges.max = 2.0f;
            p.description = "Brightness CC amount";
            break;
        case kParamBrightCc:
            p.hints |= kParameterIsInteger;
            p.name = "Brightness CC#"; p.shortName = "BrCC"; p.symbol = "brightcc";
            p.ranges.max = 127.0f;
            break;
        case kParamLoudAmt:
            p.name = "Loudness Amt"; p.shortName = "LuA"; p.symbol = "loudamt";
            p.ranges.max = 2.0f;
            p.description = "Loudness CC amount";
            break;
        case kParamLoudCc:
            p.hints |= kParameterIsInteger;
            p.name = "Loudness CC#"; p.shortName = "LuCC"; p.symbol = "loudcc";
            p.ranges.max = 127.0f;
            break;
        case kParamVelNorm:
            p.hints |= kParameterIsInteger;
            p.name = "Velocity Target"; p.shortName = "VelT"; p.symbol = "velnorm";
            p.ranges.max = 127.0f;
            p.description = "Normalize velocities towards this target (0 = off)";
            break;
        case kParamOutChan:
            p.hints |= kParameterIsInteger;
            p.name = "Out Ch"; p.shortName = "Ch"; p.symbol = "outchan";
            setEnumValues(p, 16, kChannelValues);
            p.ranges.min = 1.0f;
            p.ranges.max = 16.0f;
            p.description = "Output MIDI channel (when no split)";
            break;
        case kParamSource:
            p.hints |= kParameterIsInteger;
            p.name = "Source"; p.shortName = "Src"; p.symbol = "source";
            p.ranges.min = 0.0f;
            p.ranges.max = 2.0f;
            p.description = "0 = audio tracker (guitar in), 1 = MIDI input, 2 = both";
            setEnumValues(p, 3, kSourceValues);
            break;
        case kParamTrackLow:
            p.hints |= kParameterIsInteger;
            p.name = "Lowest Note"; p.shortName = "Low"; p.symbol = "tracklow";
            p.ranges.min = 33.0f;
            p.ranges.max = 60.0f;
            p.description = "Lowest note the audio tracker accepts (lower notes cost more latency)";
            break;
        case kParamTrackRes:
            p.hints |= kParameterIsInteger;
            p.name = "Pitch Res"; p.shortName = "Res"; p.symbol = "trackres";
            p.ranges.min = 0.0f;
            p.ranges.max = 3.0f;
            p.description = "Pitch resolution 0..3 (higher = longer analysis window, more latency)";
            break;
        case kParamTrackBright:
            p.hints |= kParameterIsInteger;
            p.name = "Track Tilt"; p.shortName = "Tilt"; p.symbol = "trackbright";
            p.ranges.min = 0.0f;
            p.ranges.max = 4.0f;
            p.description = "Brightness compensation for the poly tracker (2 = neutral, higher favours upper harmonics)";
            break;
        case kParamTrackJitter:
            p.hints |= kParameterIsInteger;
            p.name = "Track Jitter"; p.shortName = "Jit"; p.symbol = "trackjitter";
            p.ranges.min = 0.0f;
            p.ranges.max = 3.0f;
            p.description = "Stability / jitter removal: agreeing analyses required before a note commits (0 = fastest)";
            break;
        case kParamBendTrig:
            p.hints |= kParameterIsInteger;
            p.name = "Bend / Trig"; p.shortName = "B/T"; p.symbol = "bendtrig"; p.unit = "%";
            p.ranges.min = 0.0f;
            p.ranges.max = 100.0f;
            p.description = "Pitch movement handling: 0 = retrigger notes (triggers), 100 = bend the sounding note (bends)";
            break;
        case kParamBypass:
            p.hints |= kParameterIsBoolean;
            p.initDesignation(kParameterDesignationBypass);
            break;
        default:
            break;
        }
    }

    void initProgramName(const uint32_t index, String& name) override
    {
        if (index < preset_data::kCount)
            name = preset_data::kEntries[index].name;
    }

    void loadProgram(const uint32_t index) override
    {
        if (index >= preset_data::kCount)
            return;
        for (uint32_t i = 0; i < kParamCount; ++i)
            fParams[i] = (i == kParamBypass) ? fParams[i]
                                             : preset_data::kEntries[index].value[i];
    }

    float getParameterValue(const uint32_t index) const override
    {
        return index < kParamCount ? fParams[index] : 0.0f;
    }

    void setParameterValue(const uint32_t index, float value) override
    {
        if (index >= kParamCount)
            return;
        const bool wasBypassed = fParams[kParamBypass] > 0.5f;
        fParams[index] = value;
        if (index == kParamBypass) {
            const bool nowBypassed = value > 0.5f;
            if (nowBypassed && !wasBypassed)
                markFlush();      // leaving: nothing sounds
            if (!nowBypassed && wasBypassed)
                fFrame = 0.0;     // re-entering: fresh glide clock
        }
    }

    void activate() override
    {
        markFlush();
        fFrame = 0.0;
        fTracker.configure(getSampleRate());
        fLastSource = -1;
        fLastPoly = false;
    }

    void sampleRateChanged(double newSampleRate) override
    {
        fTracker.configure(newSampleRate);
    }

    void deactivate() override
    {
        markFlush();
    }

    void run(const float** inputs, float** outputs, const uint32_t frames,
             const MidiEvent* midiEvents, const uint32_t midiEventCount) override
    {
        // transparent stereo passthrough (kept running while bypassed so
        // re-engaging is seamless); hosts may pass the same buffer in and
        // out, so only copy when they differ
        if (frames != 0) {
            if (outputs[0] != inputs[0])
                std::memcpy(outputs[0], inputs[0], frames * sizeof(float));
            if (outputs[1] != inputs[1])
                std::memcpy(outputs[1], inputs[1], frames * sizeof(float));
        }
        if (frames == 0)
            return;

        const bool bypassed = fParams[kParamBypass] > 0.5f;
        const double blockStart = fFrame;
        const double blockEnd = fFrame + frames;

        // deferred flush (writeMidiEvent is only valid inside run()):
        // silence everything when bypass engaged, or on activate
        if (fPendingFlush) {
            for (auto& v : fVoices)
                if (v.active) {
                    writeMidiEvent(encodeNote(v.ch, v.pitch, 0, 0));
                    if (v.chord1) writeMidiEvent(encodeNote(v.ch, v.chord1, 0, 0));
                    if (v.chord2) writeMidiEvent(encodeNote(v.ch, v.chord2, 0, 0));
                    if (v.oct1)   writeMidiEvent(encodeNote(v.ch, v.oct1, 0, 0));
                    v.reset();
                }
            fPrimary = nullptr;
            fPendingFlush = false;
        }

        if (!bypassed)
            advanceVoices(frames, blockStart, blockEnd);

        // tracker settings (recovered from the original's tracker page)
        fTracker.setRange(static_cast<int>(std::lround(fParams[kParamTrackLow])),
                          static_cast<int>(std::lround(fParams[kParamTrackRes])));
        fTracker.stableNeed = 2 + static_cast<int>(std::lround(fParams[kParamTrackJitter]));
        fTracker.brightTilt =
            0.8f + 0.1f * static_cast<float>(std::lround(fParams[kParamTrackBright]));
        {
            const float bt = fParams[kParamBendTrig] / 100.0f;
            const float range = fParams[kParamBend];
            float bw = 0.2f + 2.6f * bt;   // 50% -> 1.5 semitones
            if (range <= 0.0f || bw > range)
                bw = range;
            fTracker.bendWindow = bw;
        }

        const int src = static_cast<int>(std::lround(fParams[kParamSource]));
        const bool poly = fParams[kParamMode] > 0.5f;
        if (src != fLastSource || poly != fLastPoly) {
            fTracker.flush([this](int n, uint32_t f) { handleNoteOff(0, n, f); });
            fTracker.reset();
            fLastSource = src;
            fLastPoly = poly;
        }

        if (bypassed) {
            if (fTracker.sounding || fTracker.pvCount > 0) {
                fTracker.flush([this](int n, uint32_t f) { handleNoteOff(0, n, f); });
                fTracker.reset();
            }
            for (uint32_t i = 0; i < midiEventCount; ++i)
                writeMidiEvent(midiEvents[i]);   // untouched passthrough
            fFrame = blockEnd;
            return;
        }

        // audio tracker (mono); its notes go through the same engine, so
        // gate, legato, stealing, scale, split etc. apply unchanged
        if (src == 0 || src == 2) {
            const float gateVel = fParams[kParamGate] * 127.0f;
            fTracker.processAudio(
                inputs[0], inputs[1], frames, blockStart, gateVel, poly,
                [this](int note, int vel, uint32_t frame, double bs) {
                    handleNoteOn(0, note, vel, frame, bs);
                },
                [this](int note, float cents, uint32_t) {
                    handleTrackerPitch(note, cents);
                },
                [this](int note, uint32_t frame) {
                    handleNoteOff(0, note, frame);
                });
        }

        if (src != 0) {
            for (uint32_t i = 0; i < midiEventCount; ++i) {
                const MidiEvent& me = midiEvents[i];
                const MidEv ev = decodeMidi(me);
                switch (ev.type) {
                case kNoteOn:
                    if (ev.d2 == 0)
                        handleNoteOff(ev.ch, ev.d1, ev.frame);
                    else
                        handleNoteOn(ev.ch, ev.d1, ev.d2, ev.frame, blockStart);
                    break;
                case kNoteOff:
                    handleNoteOff(ev.ch, ev.d1, ev.frame);
                    break;
                case kControl:
                    // the player's CCs pass through (channel remapped)
                    writeMidiEvent(encodeControl(playerChannel(), ev.d1, ev.d2, ev.frame));
                    break;
                case kPitchBend:
                    // the player's wheel passes through (channel remapped)
                    writeMidiEvent(encodePitchBend(playerChannel(),
                                                   (ev.d2 << 7) | ev.d1, ev.frame));
                    break;
                default:
                    writeMidiEvent(me);
                    break;
                }
            }
        }
        fFrame = blockEnd;
    }

private:
    // ----------------------------------------------------------------
    // channel routing
    // ----------------------------------------------------------------
    // 1-based channel from a parameter (clamped to 1..16)
    static int paramChannel(float value)
    {
        int c = static_cast<int>(value);
        if (c < 1) c = 1;
        if (c > 16) c = 16;
        return c;
    }

    // 0-based wire channel for a given (transformed) pitch
    int splitChannel(int pitch) const
    {
        const int split = static_cast<int>(fParams[kParamSplit]);
        int c;
        if (split > 0)
            c = (pitch < split) ? paramChannel(fParams[kParamLowsChan])
                                : paramChannel(fParams[kParamHighsChan]);
        else
            c = paramChannel(fParams[kParamOutChan]);
        return c - 1;
    }

    // channel for the player's own CCs/wheels: follow the last note
    int playerChannel() const
    {
        return splitChannel(fLastPitch);
    }

    // ----------------------------------------------------------------
    // voices
    // ----------------------------------------------------------------
    // note offs carry the incoming (pre-transform) pitch
    Voice* findVoice(uint8_t ch, int rawPitch)
    {
        for (auto& v : fVoices)
            if (v.active && v.ch == ch && v.rawPitch == rawPitch)
                return &v;
        return nullptr;
    }

    // mono: the one sounding voice takes over (steal < 0, any channel)
    // poly: the nearest voice on the same channel within the steal range
    Voice* findVictim(uint8_t ch, int finalPitch, int steal)
    {
        Voice* best = nullptr;
        int bestDist = 1000;
        for (auto& v : fVoices) {
            if (!v.active)
                continue;
            if (steal < 0)
                return &v;
            if (v.inCh != ch)
                continue;
            int d = finalPitch - v.pitch;
            if (d < 0) d = -d;
            if (d <= steal && d < bestDist) {
                best = &v;
                bestDist = d;
            }
        }
        return best;
    }

    Voice* claimVoice()
    {
        for (auto& v : fVoices)
            if (!v.active)
                return &v;
        // pool exhausted: steal the last voice (documented simplification)
        Voice* v = &fVoices[K_VOICES - 1];
        releaseVoice(*v, 0);
        return v;
    }

    void releaseVoice(Voice& v, uint32_t frame)
    {
        if (!v.active)
            return;
        writeMidiEvent(encodeNote(v.ch, v.pitch, 0, frame));
        if (v.chord1) writeMidiEvent(encodeNote(v.ch, v.chord1, 0, frame));
        if (v.chord2) writeMidiEvent(encodeNote(v.ch, v.chord2, 0, frame));
        if (v.oct1)   writeMidiEvent(encodeNote(v.ch, v.oct1, 0, frame));
        v.reset();
        if (fPrimary == &v)
            fPrimary = nullptr;
    }

    // writeMidiEvent is only valid inside run(), so flushing is deferred
    // to the start of the next block
    void markFlush()
    {
        fPendingFlush = true;
    }

    bool inScale(int pitch) const
    {
        const int s = static_cast<int>(fParams[kParamScale]);
        if (s < 1 || s > 17)
            return true;
        const int rel = (pitch - static_cast<int>(fParams[kParamRoot])) % 12;
        const int ir = rel < 0 ? rel + 12 : rel;
        return (kScaleMasks[s] >> ir) & 1u;
    }

    // ----------------------------------------------------------------
    // per-block voice advancement: glide landings + vibrato pitch bend
    // ----------------------------------------------------------------
    void advanceVoices(uint32_t frames, double blockStart, double blockEnd)
    {
        const double sr = getSampleRate();
        const double bendRange = fParams[kParamBend];
        const bool vibOn = fParams[kParamVibDepth] > 0.0f && bendRange > 0.0f;
        const double vibRate  = fParams[kParamVibRate];
        const double vibDepth = fParams[kParamVibDepth];

        for (auto& v : fVoices) {
            if (!v.active)
                continue;

            // glide landing
            if (v.gliding && blockEnd - v.glideStart >= v.glideDur) {
                const double land = v.glideStart + v.glideDur - blockStart;
                const uint32_t lf = (land >= frames) ? frames - 1
                                                     : static_cast<uint32_t>(land);
                writeMidiEvent(encodeNote(v.fromCh, v.fromPitch, 0, lf));
                writeMidiEvent(encodeNote(v.ch, v.toPitch, v.vel, lf));
                v.pitch = v.toPitch;
                v.fromPitch = v.toPitch;
                v.gliding = false;
                // companions land with the note
                if (v.pendOct) {
                    const int ov = clampVel(v.vel * static_cast<int>(fParams[kParamOctDown] * 10.0f) / 10);
                    writeMidiEvent(encodeNote(v.ch, v.pendOct, ov, lf));
                    v.oct1 = v.pendOct;
                    v.pendOct = 0;
                }
                emitChordTones(v, v.vel, v.pendChord1, v.pendChord2, lf);
            }

            // vibrato phase advance (original: sin(t*2*pi*freq)*cents/100)
            if (vibOn)
                v.vibPhase += 2.0 * M_PI * vibRate * frames / sr;

            // pitch bend at the block end - only the primary voice owns
            // the wheel (MPE-per-voice is not reproducible without the
            // original's MPE instrumentation; documented in the README)
            if (fPrimary == nullptr || &v != fPrimary)
                continue;

            double semi = 0.0;
            if (v.gliding) {
                const double p = (blockEnd - v.glideStart) / v.glideDur;
                if (p < 1.0)
                    // linear ramp (the original's ramp shape is not
                    // recoverable from the binary; linear is documented
                    // as an assumption)
                    semi += static_cast<double>(v.toPitch - v.fromPitch) * p;
            }
            if (vibOn)
                semi += std::sin(v.vibPhase) * (vibDepth / 100.0);
            if (bendRange > 0.0)
                semi += v.bendOffset;
            if (bendRange <= 0.0)
                continue;   // no bend range configured

            if (semi != 0.0 || v.gliding || v.lastBend != 8192) {
                // 14-bit pitch bend: the bend range parameter is the
                // assumed wheel span (the original: "Set Bend according
                // to the device to produce correct pitches")
                const double scale = 8192.0 / bendRange;
                long raw = static_cast<long>(std::llround(8192.0 + semi * scale));
                if (raw < 0) raw = 0;
                if (raw > 16383) raw = 16383;
                const bool due = (raw != v.lastBend) &&
                    ((blockEnd - v.lastBendFrame >= 128.0) ||
                     (std::abs(static_cast<double>(raw) - v.lastBend) >= 16.0));
                if (due) {
                    v.lastBend = static_cast<int>(raw);
                    v.lastBendFrame = blockEnd;
                    fLastBendOut = v.lastBend;
                    writeMidiEvent(encodePitchBend(v.ch, v.lastBend, frames - 1));
                }
            }
        }
    }

    // ----------------------------------------------------------------
    // note handling
    // ----------------------------------------------------------------
    void handleNoteOn(uint8_t ch, int pitch, int vel, uint32_t frame, double blockStart)
    {
        // gate: "Notes will trigger when the indicator is above the gate."
        // (dropped: the note never reaches the downstream instrument)
        if (vel < fParams[kParamGate] * 127.0f)
            return;

        // pitch transforms
        const int shift = static_cast<int>(fParams[kParamChordShift])
                        + static_cast<int>(fParams[kParamTranspose]);
        const int finalPitch = pitch + shift;

        // scale filter: "Only pass through pitches in the scale"
        if (!inScale(finalPitch))
            return;

        // velocity normalize (estimated law: scale toward the target)
        const float vn = fParams[kParamVelNorm];
        if (vn > 0.0f) {
            vel = static_cast<int>(std::llround(vel * vn / 127.0f));
            if (vel < 1) vel = 1;
            if (vel > 127) vel = 127;
        }

        fLastPitch = finalPitch;
        const uint8_t outCh = static_cast<uint8_t>(splitChannel(finalPitch));

        // a tracker bend may still be applied; recentre before the new note
        if (fLastBendOut != 8192) {
            writeMidiEvent(encodePitchBend(outCh, 8192, frame));
            fLastBendOut = 8192;
        }

        const int steal = static_cast<int>(fParams[kParamSteal]);
        const bool mono = fParams[kParamMode] < 0.5f;

        Voice* victim = findVictim(ch, pitch, mono ? -1 : steal);

        if (victim != nullptr && canGlide(*victim, finalPitch)) {
            startGlide(*victim, ch, outCh, pitch, finalPitch, vel, frame, blockStart);
        }
        else if (victim != nullptr) {
            // hard retrigger (no bends allowed, no legato, or out of range)
            releaseVoice(*victim, frame);
            Voice& v = *claimVoice();
            startVoice(v, ch, outCh, pitch, finalPitch, vel, frame);
        }
        else {
            Voice& v = *claimVoice();
            startVoice(v, ch, outCh, pitch, finalPitch, vel, frame);
        }
    }

    bool canGlide(Voice& v, int finalPitch) const
    {
        const int delta = finalPitch - v.pitch;
        return fParams[kParamLegato] > 0.5f
            && fParams[kParamBend] > 0.0f
            && delta != 0
            && std::abs(delta) <= fParams[kParamBend];
    }

    void startVoice(Voice& v, uint8_t inCh, uint8_t outCh, int rawPitch, int pitch,
                    int vel, uint32_t frame)
    {
        v.reset();
        v.active = true;
        v.inCh = inCh;
        v.ch = outCh;
        v.pitch = pitch;
        v.rawPitch = rawPitch;
        v.vel = vel;
        v.vibPhase = std::uniform_real_distribution<double>(0.0, 2.0 * M_PI)(fRand);
        fPrimary = &v;
        beginChord(v, pitch, vel, frame);
        beginOctaveDown(v, pitch, vel, frame);
        emitDynamicsCC(v, pitch, frame);
        writeMidiEvent(encodeNote(outCh, pitch, vel, frame));
    }

    void startGlide(Voice& v, uint8_t inCh, uint8_t outCh, int rawPitch, int finalPitch,
                    int vel, uint32_t frame, double blockStart)
    {
        // release the old companions
        if (v.chord1) writeMidiEvent(encodeNote(v.ch, v.chord1, 0, frame));
        if (v.chord2) writeMidiEvent(encodeNote(v.ch, v.chord2, 0, frame));
        if (v.oct1)   writeMidiEvent(encodeNote(v.ch, v.oct1, 0, frame));
        v.chord1 = v.chord2 = v.oct1 = 0;

        v.fromCh = v.ch;
        v.fromPitch = v.pitch;
        v.toPitch   = finalPitch;
        v.gliding   = true;
        v.vel       = vel;
        v.inCh      = inCh;
        v.ch        = outCh;
        v.rawPitch  = rawPitch;
        v.glideStart = blockStart + frame;
        const double ms = fParams[kParamLegatoMs];
        v.glideDur = std::max(2.0, ms * getSampleRate() / 1000.0);
        v.vibPhase = std::uniform_real_distribution<double>(0.0, 2.0 * M_PI)(fRand);
        v.lastBend = 8192;
        v.lastBendFrame = v.glideStart;
        fPrimary = &v;

        // the new note's companions land with the glide
        beginChord(v, finalPitch, vel, 0, /*gliding=*/true);
        beginOctaveDown(v, finalPitch, vel, 0, /*gliding=*/true);
        emitDynamicsCC(v, finalPitch, frame);

        // the note attacks at the OLD pitch on the new output channel; the
        // glide landing swaps the notes (the original's deferred-ms behaviour)
        writeMidiEvent(encodeNote(outCh, v.fromPitch, vel, frame));
    }

    // chord-wheel companions of a freshly struck note: the wheel advances
    // on each fresh note (major -> minor -> diminished), mirroring the
    // original's "pitch shift among three chords"
    void beginChord(Voice& v, int root, int vel, uint32_t frame, bool gliding = false)
    {
        const int wheel = static_cast<int>(fParams[kParamChordWheel]);
        if (wheel < 1)
            return;
        fWheelIdx = (fWheelIdx + 1) % 3;
        const int* iv = kChordVecs[fWheelIdx];
        int c1 = 0, c2 = 0;
        if (inScale(root + iv[1])) c1 = root + iv[1];
        if (inScale(root + iv[2])) c2 = root + iv[2];
        v.pendChord1 = c1;
        v.pendChord2 = c2;
        if (!gliding)
            emitChordTones(v, vel, c1, c2, frame);
    }

    void emitChordTones(Voice& v, int vel, int c1, int c2, uint32_t frame)
    {
        // companion velocities are a documented estimate (the original's
        // weighting is not recoverable from the binary)
        v.chord1 = 0;
        v.chord2 = 0;
        if (c1) {
            writeMidiEvent(encodeNote(v.ch, c1, clampVel(vel * 8 / 10), frame));
            v.chord1 = c1;
        }
        if (c2) {
            writeMidiEvent(encodeNote(v.ch, c2, clampVel(vel * 6 / 10), frame));
            v.chord2 = c2;
        }
    }

    // octave-down: "Also play a note an octave lower with half the velocity"
    void beginOctaveDown(Voice& v, int root, int vel, uint32_t frame, bool gliding = false)
    {
        const float mult = fParams[kParamOctDown];
        if (mult <= 0.0f)
            return;
        const int ovr = clampVel(vel * static_cast<int>(mult * 10.0f) / 10);
        const int opitch = root - 12;
        v.pendOct = opitch;
        if (!gliding) {
            writeMidiEvent(encodeNote(v.ch, opitch, ovr, frame));
            v.oct1 = opitch;
            v.pendOct = 0;
        }
    }

    // velocity-driven dynamics CC (original: per-frame CC from the
    // measured picking dynamics on audio input; here velocity is the
    // proxy - a documented estimate). Sent once per note on; the host's
    // downstream instrument holds the value for the note's life.
    void emitDynamicsCC(Voice& v, int pitch, uint32_t frame)
    {
        const float brightAmt = fParams[kParamBrightAmt];
        const float loudAmt   = fParams[kParamLoudAmt];
        const float nv = static_cast<float>(v.vel) / 127.0f;
        const int ch = splitChannel(pitch);
        if (brightAmt > 0.0f) {
            // "brightness = min(1, n.brightness * amount)"
            float b = nv * brightAmt;
            if (b > 1.0f) b = 1.0f;
            writeMidiEvent(encodeControl(ch, static_cast<int>(fParams[kParamBrightCc]),
                                   static_cast<int>(std::llround(127.0f * b)), frame));
        }
        if (loudAmt > 0.0f) {
            float l = nv * loudAmt;
            if (l > 1.0f) l = 1.0f;
            writeMidiEvent(encodeControl(ch, static_cast<int>(fParams[kParamLoudCc]),
                                   static_cast<int>(std::llround(127.0f * l)), frame));
        }
    }

    // continuous pitch from the audio tracker (MONO): a deviation inside
    // the bend window updates the sounding note's bend instead of
    // retriggering it
    void handleTrackerPitch(int rawPitch, float cents)
    {
        if (fParams[kParamBend] <= 0.0f)
            return;
        double semis = cents / 100.0;
        const double lim = fTracker.bendWindow;
        if (semis > lim) semis = lim;
        if (semis < -lim) semis = -lim;
        for (auto& v : fVoices)
            if (v.active && v.inCh == 0 && v.rawPitch == rawPitch && !v.gliding) {
                v.bendOffset = semis;
            }
    }

    void handleNoteOff(uint8_t ch, int rawPitch, uint32_t frame)
    {
        // release every voice matching the incoming note (duplicates can
        // exist when MIDI and the audio tracker both play the same pitch)
        for (auto& v : fVoices)
            if (v.active && v.inCh == ch && v.rawPitch == rawPitch && !v.gliding)
                releaseVoice(v, frame);
    }

    static int clampVel(int v)
    {
        if (v < 1) v = 1;
        if (v > 127) v = 127;
        return v;
    }

    static constexpr int K_VOICES = 24;
    Voice fVoices[K_VOICES];
    float fParams[kParamCount];
    double fFrame = 0.0;       // absolute frame counter
    int fWheelIdx = 0;
    bool fPendingFlush = false;
    Voice* fPrimary = nullptr; // the voice owning the pitch-bend wheel
    int fLastPitch = 60;       // for routing the player's CCs/wheels
    std::mt19937 fRand;
    AudioTracker fTracker;
    int fLastSource = -1;
    bool fLastPoly = false;
    int fLastBendOut = 8192;

    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginStringler)
};

Plugin* createPlugin()
{
    return new PluginStringler();
}

END_NAMESPACE_DISTRHO
