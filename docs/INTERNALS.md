# Stringler internals

This document describes how the engine works: the audio tracker, the note
filtering, voice allocation, the expression stages, how they interact, and
what each control does. It matches the code in
`plugins/Stringler/PluginStringler.cpp`; if the two disagree, the code wins.

## Contents

1. [Signal flow](#1-signal-flow)
2. [Sources and event normalisation](#2-sources-and-event-normalisation)
3. [The audio tracker](#3-the-audio-tracker)
4. [Note filtering and pitch transforms](#4-note-filtering-and-pitch-transforms)
5. [Voice allocation](#5-voice-allocation)
6. [Expression stages](#6-expression-stages)
7. [Channels, split and bypass](#7-channels-split-and-bypass)
8. [Control reference](#8-control-reference)
9. [Interactions and worked examples](#9-interactions-and-worked-examples)
10. [Timing and latency](#10-timing-and-latency)
11. [Implementation notes](#11-implementation-notes)

---

## 1. Signal flow

Stringler is a MIDI processor with an audio input used only for tracking.
The audio itself is always copied input → output unchanged.

```
audio in ──► tracker (mono or poly) ─┐
                                     ├─► note filtering ──► voice allocation
MIDI in ─────────────────────────────┘        (gate, shift, scale,           │
                                               velocity target)              ▼
                                                                    expression stages
                                                          (glide, vibrato, tracker
                                                           bends, chord wheel,
                                                           octave down, CCs)
                                                                             │
                                                                             ▼
                                                         channel routing (split/out)
                                                                             │
                                                                             ▼
                                                                         MIDI out
```

Sources are mixed at the *event* level: the tracker emits note events that go
through exactly the same pipeline as incoming MIDI. All stage ordering below is
the order in `run()` / `handleNoteOn()`.

## 2. Sources and event normalisation

* **Source** (`source`, 0–2) selects which inputs are processed:
  `Audio` (tracker only), `MIDI` (incoming MIDI only) or `Both` (default).
  Changing the source flushes the tracker and resets its history.
* **MIDI input** is decoded from raw `MidiEvent`s: note on/off, control change
  and pitch bend. Note on with velocity 0 is treated as a note off.
* **Tracker notes** are injected as note on/off on input channel 0, with the
  detected pitch as the raw pitch and the level-derived velocity.
* Voices remember both the **input channel** (used to match note offs and to
  scope polyphonic stealing) and the **output channel** (where the note is
  finally emitted).
* A note off releases **every** active voice with the same input channel and raw
  pitch. Duplicates are therefore handled explicitly: the tracker and incoming
  MIDI can both play the same pitch.
* With MIDI enabled, the player's control changes and pitch bend are passed
  through on the channel of the most recent note (so CCs follow the note
  routing); unknown messages pass through untouched.

## 3. The audio tracker

The tracker is level-driven, not spectrum-driven at the note level: it derives
a velocity from the input level and hands notes to the same pipeline as MIDI.
The only audio-specific threshold is therefore **Gate**, which acts as the
input-level threshold (see §4).

### 3.1 Input analysis and level

* The two input channels are summed to mono: `x = (L + R) / 2`.
* A window of samples is analysed; the RMS of that window is converted to dB
  (`20·log10(rms)`).
* **Velocity mapping**: `-60 dB → 0`, `0 dB → 127`, linear in dB:
  `vel = round(127 · (dB + 60) / 60)`, clamped to 0…127.
* A note is only considered when `dB > -70` **and** `vel ≥ Gate · 127`. Because
  the note then passes through the normal gate in §4 with the same velocity,
  Gate has one consistent meaning for both sources.
* Release: the note is released after two consecutive analyses below the gate
  (mono) or two missed analyses (poly), so a short dip does not cut a note.

### 3.2 Mono detector (`mode = Mono`)

A YIN-style periodicity detector, FFT-accelerated:

1. Window length `W` is chosen from the **lowest note** and **pitch
   resolution**: `W = periods · lagMax + 32`, where `lagMax = sr / f(lowest)`
   and `periods = 3.0 + 0.5 · resolution` (3.0…4.5 periods).
2. The window is zero-padded to the next power of two and transformed. The
   autocorrelation comes from `|X(f)|²` (forward, power, inverse), which gives
   the difference function `d(τ) = m₀ + m(τ) − 2·acf(τ)` using suffix sums of
   squares for `m(τ)`.
3. The **cumulative mean normalised difference** `cmndf(τ)` is computed over
   `lagMin = sr/1200` … `lagMax`. The first `τ` with `cmndf < 0.12` is taken
   (walking to the local minimum); if nothing dips below that but the global
   minimum is below `0.25`, the global minimum is used, otherwise the analysis
   is rejected as unpitched.
4. Parabolic interpolation refines `τ`, and `f0 = sr / τ` is converted to a
   MIDI note (`69 + 12·log₂(f0/440)`, rounded). Notes outside the tracker's
   range (`lowest·0.94` … 1500 Hz, and MIDI 24…100) are rejected.
5. **Commit hysteresis**: a note must be seen by `stableNeed = 2 + jitter`
   consecutive analyses before it is emitted (jitter 0…3 → 2…5 analyses).

Analyses run once per **hop** of `round(sr/187.5)` samples — 256 samples
(≈5.3 ms) at 48 kHz.

**Continuous pitch (bends).** While a note is sounding, the deviation from the
tracked note is compared with the **bend window** (see Bend/Trig in §8). Inside
the window the pitch is reported as a bend in cents and no new note is
triggered; outside it, the note is committed as a new note (subject to the
stability rule). This is the behaviour the original exposes as its
"note bend vs triggers" tweak.

### 3.3 Poly detector (`mode = Poly`)

A harmonic-comb multi-pitch detector over a longer window:

* **Window/cadence**: ≈85 ms Hann-windowed analysis (`window2`), transformed
  on a 2× zero-padded grid, run on every **fourth** hop (≈21 ms). The extra
  latency buys the frequency resolution needed for low chord tones.
* **Spectrum preparation**: magnitudes are compressed with a square root
  (fourth root of power). This keeps real peaks far above the numerical noise
  floor without amplifying empty bins (a whitening attempt did exactly that).
* **Peaks**: local maxima over ±2 bins, at least 20 % of the strongest peak,
  with parabolic frequency interpolation. The floor rejects Hann sidelobes,
  which otherwise look like harmonics of higher notes.
* **Scoring**: for every candidate note from **Lowest Note** to E6 (MIDI 88),
  harmonics 1…8 are matched to peaks within **±60 cents** and summed with
  weights `1.0` (fundamental), `0.5` (odd harmonics), `0.3` (even harmonics),
  each scaled by `tilt^(h−1)` (see Track Tilt). The comb over high harmonics
  is what resolves low fundamentals: their own spacing is too coarse, but
  their harmonics are not.
* **Selection**: up to four notes per analysis. The first pick must score at
  least 1.0; later picks must reach 25 % of the first. After each pick, the
  peaks belonging to that note's harmonic series are **retired**, which
  suppresses octave doublings (a real octave double of a picked note is
  therefore merged into the lower note).
* **Voice tracking**: new notes need `stableNeed` agreeing analyses as in mono;
  notes disappear after two missed analyses.
* Velocity for all notes of a chord is the overall level-derived velocity.

### 3.4 Tracker settings overview

| Control | Effect | Cost |
|---|---|---|
| Lowest Note | sets `lagMax` → window length; notes below it are not tracked | lower notes = more latency |
| Pitch Res | analysis length 3.0…4.5 periods | higher = more latency |
| Track Tilt | harmonic weight tilt `0.8 + 0.1·tilt` per harmonic (poly only) | none |
| Track Jitter | `stableNeed = 2 + jitter` | higher = later notes, steadier pitch |
| Bend/Trig | bend window `0.2 + 2.6·(value/100)`, capped by Bend (mono only) | none |

### 3.5 Tracker limitations (by design)

* Mono is the reliable path; poly is experimental on dense or distorted
  material and merges octave doublings.
* Poly is note-level only: no per-note bends or vibrato.
* Tracker bends are mono-only, and the YIN estimate is note-level apart from
  the cents offset reported inside the bend window.
* The tracker resets when the source changes (Audio/MIDI/Both) or between mono
  and poly, so switching never leaks held notes.

## 4. Note filtering and pitch transforms

Every note on — from the tracker or from MIDI — passes these stages in order:

1. **Gate**: `if (vel < Gate · 127) drop`. The note never reaches the
   downstream instrument (and for tracker notes this is also the input-level
   threshold, because velocity is level-derived).
2. **Pitch transforms**: `finalPitch = rawPitch + Chord Shift + Transpose`.
   Chord Shift (−12…+12) and Transpose (−24…+24) are summed; both are
   chromatic.
3. **Scale filter**: 0 = off. 1…17 selects one of the original's seventeen
   scales (major, minor, harmonic/melodic minor, the modes, Spanish, Flamenco,
   blues, chromatic). The mask is a bit per pitch class, rotated by Root:
   `rel = (pitch − Root) mod 12`, keep if the bit is set. Out-of-scale notes
   are dropped. The filter applies to the **final** pitch, i.e. after shift and
   transpose, and it also filters the chord-wheel companions (§6.4).
4. **Velocity normalisation**: if the target is non-zero,
   `vel = round(vel · target / 127)`, clamped to 1…127. This happens *after*
   the gate, so a note that passed the gate is not re-gated by normalisation.
5. The final pitch is stored as "last pitch" for the player's CC/bend channel.

## 5. Voice allocation

The engine keeps a pool of **24 voices**. A voice holds: input channel, output
channel, raw pitch (for note-off matching), transformed pitch, velocity,
companions (two chord tones and one octave-down tone), glide state and
per-voice bend state.

* **Mono** (`mode = Mono`): a new note takes over the sounding voice. Takeover
  is a glide if legato and the bend range allow it (§6.1), otherwise a hard
  retrigger: the old voice is released and a new one started.
* **Poly** (`mode = Poly`): a new note steals the nearest active voice **on the
  same input channel** whose transformed pitch is within **Steal** semitones;
  otherwise it takes a free voice. If the pool is exhausted, it releases the
  last voice and takes it (a documented simplification).
* A note off releases all voices matching (input channel, raw pitch) — except
  gliding voices, whose old pitch is still sounding; their note off is sent at
  the landing (§6.1).
* Releasing a voice sends note offs for its root **and its companions** on the
  voice's output channel.

## 6. Expression stages

### 6.1 Legato glide

A new note glides when all of these hold: Legato is on, Bend > 0, the
transformed interval is non-zero and `|Δ| ≤ Bend`.

1. The voice keeps its old pitch as `fromPitch` and its old output channel as
   `fromCh`, and adopts the new raw pitch, transformed pitch and output
   channel.
2. A note on is emitted **at the old pitch** with the new velocity, so the
   downstream instrument attacks immediately instead of waiting for the glide.
3. A pitch bend ramps linearly from the old to the new pitch over
   `Glide Time` milliseconds (`glideDur = ms · sr / 1000`), written at block
   ends with a rate limit (see §6.2).
4. At the landing the engine sends a note off for the old pitch on `fromCh`, a
   note on for the new pitch on the output channel, and emits the deferred
   companions (chord tones and octave-down). If the landing falls beyond the
   current block, it is emitted in a later block.

With Bend = 0 there are no glides (this is the "no bends" case), and with
Legato off a new note always retriggers.

### 6.2 Automatic vibrato and bend rate limiting

* Vibrato is a sine on the pitch wheel: `sin(phase) · depth/100` semitones;
  `phase += 2π · rate · frames / sr`, with a random start phase per note.
* In this DPF port only the **primary voice** owns the wheel: bends are sent on
  that voice's channel only. Per-note (MPE) wheels are not reproduced; this is
  documented as a limitation.
* Bend writes are rate-limited: only when the value changes **and** either 128
  frames have passed or the value moved by at least 16 units (14-bit).
* A tracker bend offset (§6.3) is summed with the glide and vibrato terms.
* The wheel is **re-centred** before a new note starts, so a previous bend does
  not leak into the new note.

### 6.3 Tracker bends

In mono, the tracker reports cents deviations inside the bend window. The
plugin finds the tracker voice (input channel 0, matching raw pitch) and stores
the deviation as that voice's bend offset, clamped to the window. This offset
flows into the same wheel write as vibrato and glide, so all three are summed.
The window is capped by Bend, and with Bend = 0 no tracker bends are applied.

### 6.4 Chord wheel

* Enabled by Chord Wheel = 1…3 (Off / Major / Minor / Diminished).
* The wheel **advances on every fresh note** (major → minor → diminished →
  major …), mirroring the original's "pitch shift among three chords".
* Intervals are taken from the original's chord table: major `{0,4,7}`, minor
  `{0,3,7}`, diminished `{0,3,8}` (as recovered from the binary; note that the
  original's "diminished" entry raises the fifth instead of lowering it).
* Companions are emitted only when they pass the **scale filter** (when a
  scale is active).
* Companion velocities are documented estimates: 80 % and 60 % of the root
  velocity.
* Companions belong to the voice and are released with it; when a glide is in
  progress they are emitted at the landing.

### 6.5 Octave-down companion

* OCT DOWN (Octave Down) is a velocity multiplier, 0 = off. The companion is
  `root − 12` with velocity `root velocity · multiplier`, rounded to 10 %
  steps. The original always plays it at half velocity; the multiplier here is
  exposed and the presets use 0, 0.5 and 1.0.
* It is released with the voice and, during a glide, emitted at the landing.

### 6.6 Brightness and loudness CCs

On every fresh note (and on a glide's attack) the engine sends up to two
control changes on the routed channel:

```
value = round(127 · min(1, (velocity / 127) · amount))
```

with the CC number from Brightness CC# / Loudness CC#. Amount 0 disables that
CC. The original derives brightness/loudness from measured picking dynamics per
audio frame; at MIDI level velocity is the available proxy (documented
estimate). The CC is sent with the note and held by the downstream instrument
for the note's life.

## 7. Channels, split and bypass

* **Routing**: if Split is non-zero, the transformed pitch decides the output
  channel: below the split pitch → Low Ch, otherwise High Ch. With split off,
  every note goes to Out Ch. Channels are 1…16 (wire 0…15).
* Note offs are always emitted on the **same output channel** as the note;
  companions follow the voice.
* The player's CCs and pitch bend follow the most recent note's routed channel.
* **Bypass** (`bypass`, exported as LV2 `enabled`): incoming MIDI is passed
  through untouched and the audio keeps flowing; the tracker is flushed and
  reset so no stale notes survive. Re-entering starts from a clean state.

## 8. Control reference

Symbols are stable identifiers (LV2 ports 6 + index; Bypass is port 35). Ranges
are the host-facing ranges; "estimate" marks values the binary does not pin
down.

### Tracker

| Symbol | Name | Range | Default | Meaning |
|---|---|---|---|---|
| `tracklow` | Lowest Note | 33–60 | E2 | lowest note the tracker accepts; lower = longer window |
| `trackres` | Pitch Res | 0–3 | 0 | analysis length 3.0–4.5 periods |
| `trackbright` | Track Tilt | 0–4 | 2 | poly harmonic tilt, 2 = neutral |
| `trackjitter` | Track Jitter | 0–3 | 0 | agreeing analyses before commit (2…5) |
| `bendtrig` | Bend/Trig | 0–100 % | 50 | bend window 0.2–2.8 st (capped by Bend) |
| `source` | Source | 0–2 | Both | Audio / MIDI / Both |

### Note pipeline

| Symbol | Name | Range | Default | Meaning |
|---|---|---|---|---|
| `mode` | Tracking Mode | Mono/Poly | Mono | voice policy and tracker model |
| `gate` | Velocity Gate | 0–1 | 0.10 | drop notes below this velocity (input level for audio) |
| `bend` | Bend Range | 0–48 st | 12 | wheel span; 0 disables glides and bends |
| `legato` | Legato | Off/On | On | allow glide takeovers |
| `legatoms` | Glide Time | 0–100 ms | 30 | glide ramp length |
| `vibdepth` | Vibrato Depth | 0–200 cents | 0 | automatic vibrato depth |
| `vibrate` | Vibrato Rate | 0–10 Hz | 5 | automatic vibrato rate |
| `chordshift` | Chord Shift | −12…+12 st | 0 | chromatic shift added to Transpose |
| `chordwheel` | Chord Wheel | Off/Major/Minor/Diminished | Off | chord-tone companions, advancing per note |
| `transpose` | Transpose | −24…+24 st | 0 | chromatic transpose |
| `scale` | Scale Filter | Off + 17 scales | Off | keep only notes in the scale |
| `root` | Root | C…B | C | scale root |
| `octdown` | Octave Down | 0–2 | 1.0 | velocity multiplier for the −12 companion (0 = off) |
| `velnorm` | Velocity Target | 0–127 | 0 | normalise velocities; 0 = off |
| `split` | Split Pitch | Off, E2…E4 | D3 | lows/highs boundary |
| `lowschan` | Low Ch | 1–16 | 1 | channel for notes below the split |
| `highschan` | High Ch | 1–16 | 1 | channel for notes at/above the split |
| `outchan` | Out Ch | 1–16 | 1 | channel when split is off |
| `brightamt` / `brightcc` | Brightness Amt / CC# | 0–2 / 0–127 | 0 / 1 | velocity-driven brightness CC |
| `loudamt` / `loudcc` | Loudness Amt / CC# | 0–2 / 0–127 | 0 / 7 | velocity-driven loudness CC |
| `steal` | Note Steal | 0–12 st | 1 | poly: steal the nearest voice within this interval |
| `bypass` | Bypass | Off/On | Off | raw passthrough |

## 9. Interactions and worked examples

* **Gate and Velocity Target.** Gate is evaluated on the incoming velocity
  (for audio: the level-derived one). Velocity Target is applied *after* the
  gate, so normalising never resurrects a gated note, but it does change what
  the downstream instrument receives and what the CCs are computed from.
  The tracker also uses the same gate to decide whether to track at all.
* **Pitch Res / Lowest Note / Jitter** all add latency and compose: the window
  grows with the lowest note and resolution, and each additional analysis
  needed by Jitter adds one hop.
* **Bend/Trig with Bend.** The tracker's bend window is
  `0.2 + 2.6·(Bend/Trig)` semitones, but never more than Bend. With Bend = 0
  there are no tracker bends and no glides; with Bend = 2 the maximum window is
  2 semitones even at 100 %.
* **Transpose/Chord Shift with Split.** The split decision uses the *transformed*
  pitch (`raw + shift + transpose`), so transposing a part can move it across
  the split boundary. The scale filter runs on the same transformed pitch.
* **Scale with Chord Wheel.** The root is filtered by Scale before allocation;
  the wheel's companions are each checked against the scale and omitted when
  out of key. A scale therefore changes which companion tones appear.
* **Legato with Steal.** In Mono, Legato and the bend range decide whether a
  takeover is a glide; Steal only affects Poly.
* **Tracker bends with Vibrato.** Both add to the same wheel value: the
  tracker's cents offset plus the sine vibrato (plus any glide ramp). The wheel
  is recentred on the next note.

### Example: one tracked note in Mono

1. Audio reaches the tracker; an analysis finds `f0 = 110.0 Hz` → MIDI 45
   (A2) and the level gives velocity 96.
2. Gate (0.1) passes 96. Transpose 0, so the final pitch stays 45.
3. Scale off, Velocity Target 0 → velocity stays 96.
4. Mono takeover: no voice is active, so a new voice starts on channel 0.
5. Octave Down 1.0 → companion A1 at velocity 96; Chord Wheel off.
6. Brightness Amt 1.0 → CC 1 = 96 is sent on the routed channel.
7. The next analysis finds 117.0 Hz (≈ +110 cents): inside the bend window, so
   a bend of +1.1 semitones is sent instead of a new note.

### Example: two-note chord in Poly

1. The poly analysis finds A2 and E3 (a perfect fifth) and retires both
   harmonic series; a second analysis confirms both.
2. Each note passes the gate and scale, and gets its own voice (Steal must be
   smaller than 7 for the fifth not to steal the root).
3. Each voice emits its own note on the routed channel and its dynamics CCs.

## 10. Timing and latency

At 48 kHz, with the default settings (Lowest Note E2, Pitch Res 0, Jitter 0):

| Path | Window | Analyses to commit | Latency |
|---|---|---|---|
| Mono | 1781 samples ≈ 37 ms | 2 hops ≈ 11 ms | **≈48 ms** |
| Poly | 4090 samples ≈ 85 ms | 2 analyses ≈ 43 ms | **≈128 ms** |

Latency scales with the tracker settings:

* lowering Lowest Note to A1 (55 Hz) takes the mono window to ≈55 ms;
* Pitch Res 3 adds another ≈50 % window;
* Jitter 3 needs five analyses (+16 ms mono, +85 ms poly).

Note off latency is deliberately shorter: two silent analyses.

## 11. Implementation notes

* MIDI writes are only legal inside `run()` in this DPF revision
  (`writeMidiEvent`). Everything that has to wait (bypass flushes, tracker
  resets) is deferred to the top of the next block.
* The tracker analysis is allocation-free after `activate()`; buffers are sized
  for the widest configuration (lowest note A1 at maximum resolution).
* One twiddle table serves every power-of-two FFT size: the table index for a
  given transform length is independent of the size, so the mono FFT shrinks
  with the configured window instead of always running at the maximum size.
* The mono analysis uses a rectangular window (YIN); the poly analysis uses
  Hann (better peak definition) with 2× zero padding.
* Programs (presets) are loaded through DPF programs; Bypass is kept out of the
  preset data by design.
* Parameters with named values are DPF enumerations, so hosts present Mono/Poly,
  scales, notes and channels as named dropdowns. The faceplate uses selector
  badges for them, and the modgui uses mod-ui's native select widget.
