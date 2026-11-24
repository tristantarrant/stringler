/*
 * Stringler - names for the discrete parameter values.
 *
 * Shared by the plugin (LV2/CLAP/VST3 enumerations), the cairo faceplate
 * (selector badges) and the modgui renderer (<select> options), so the
 * three cannot drift apart.
 *
 * Copyright (C) 2026 Dataforte
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

namespace sl_names {

struct Item { int value; const char* label; };

constexpr Item kMode[] = {
    { 0, "Mono" }, { 1, "Poly" },
};
constexpr int kModeCount = 2;

constexpr Item kLegato[] = {
    { 0, "Off" }, { 1, "On" },
};
constexpr int kLegatoCount = 2;

constexpr Item kSource[] = {
    { 0, "Audio" }, { 1, "MIDI" }, { 2, "Both" },
};
constexpr int kSourceCount = 3;

constexpr Item kWheel[] = {
    { 0, "Off" }, { 1, "Major" }, { 2, "Minor" }, { 3, "Diminished" },
};
constexpr int kWheelCount = 4;

constexpr Item kRoot[] = {
    { 0, "C" }, { 1, "C#" }, { 2, "D" }, { 3, "D#" },
    { 4, "E" }, { 5, "F" }, { 6, "F#" }, { 7, "G" },
    { 8, "G#" }, { 9, "A" }, { 10, "A#" }, { 11, "B" },
};
constexpr int kRootCount = 12;

constexpr Item kScale[] = {
    { 0, "Off" }, { 1, "Major" }, { 2, "Minor" },
    { 3, "Harmonic Minor" }, { 4, "Melodic Minor" },
    { 5, "Dorian" }, { 6, "Locrian" }, { 7, "Diminished" },
    { 8, "Augmented" }, { 9, "Spanish" }, { 10, "Flamenco" },
    { 11, "Mixolydian" }, { 12, "Lydian" }, { 13, "Lydian Minor" },
    { 14, "Lydian Augmented" }, { 15, "Lydian Dominant" },
    { 16, "Blues" }, { 17, "Chromatic" },
};
constexpr int kScaleCount = 18;

// Split pitch: Off, then the guitar-register boundary notes E2..E4
// (the original exposes split_pitch 40..64).
constexpr Item kSplit[] = {
    { 0, "Off" },
    { 40, "E2" }, { 41, "F2" }, { 42, "F#2" }, { 43, "G2" }, { 44, "G#2" },
    { 45, "A2" }, { 46, "A#2" }, { 47, "B2" },
    { 48, "C3" }, { 49, "C#3" }, { 50, "D3" }, { 51, "D#3" }, { 52, "E3" },
    { 53, "F3" }, { 54, "F#3" }, { 55, "G3" }, { 56, "G#3" }, { 57, "A3" },
    { 58, "A#3" }, { 59, "B3" },
    { 60, "C4" }, { 61, "C#4" }, { 62, "D4" }, { 63, "D#4" }, { 64, "E4" },
};
constexpr int kSplitCount = 26;

constexpr Item kChannel[] = {
    { 1, "1" }, { 2, "2" }, { 3, "3" }, { 4, "4" }, { 5, "5" }, { 6, "6" },
    { 7, "7" }, { 8, "8" }, { 9, "9" }, { 10, "10" }, { 11, "11" }, { 12, "12" },
    { 13, "13" }, { 14, "14" }, { 15, "15" }, { 16, "16" },
};
constexpr int kChannelCount = 16;

// label of a value in a table, or nullptr
inline const char* lookup(const Item* items, const int count, const int value)
{
    for (int i = 0; i < count; ++i)
        if (items[i].value == value)
            return items[i].label;
    return nullptr;
}

} // namespace sl_names
