/*
 * Stringler - scalable cairo UI (DPF).
 *
 * The pedal is drawn by Artwork.hpp in a fixed design space and scaled
 * to the window, keeping the aspect ratio. Interaction is mapped through
 * the artwork's control table:
 *   knobs        drag up/down (Shift: fine), mouse wheel, double-click
 *                resets to the default
 *   switches     click toggles MODE / LEGATO
 *   footswitch   click toggles bypass
 *
 * Copyright (C) 2026 Dataforte
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "DistrhoUI.hpp"
#include "Artwork.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>

START_NAMESPACE_DISTRHO

using DGL_NAMESPACE::CairoGraphicsContext;

static_assert(static_cast<int>(sl_art::P_COUNT) == static_cast<int>(kParamCount), "artwork and plugin parameter lists differ");

class UIStringler : public UI
{
public:
    UIStringler()
        : UI(DISTRHO_UI_DEFAULT_WIDTH, DISTRHO_UI_DEFAULT_HEIGHT)
    {
        const double s = getScaleFactor();
        if (s != 1.0)
            setSize(static_cast<uint>(sl_art::W * s), static_cast<uint>(sl_art::H * s));
        // resizable from half to any size, aspect ratio kept
        setGeometryConstraints(static_cast<uint>(sl_art::W * 0.5 * s),
                               static_cast<uint>(sl_art::H * 0.5 * s), true, false);

        for (int i = 0; i < sl_art::P_COUNT; ++i)
            fDefaults[i] = fState.value[i];
    }

protected:
    void uiIdle() override
    {
        if (fHoverParam < 0 || fTooltip)
            return;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - fHoverSince).count();
        if (ms >= 600) {
            fTooltip = true;
            repaint();
        }
    }

    // ---- host -> UI ------------------------------------------------
    void parameterChanged(const uint32_t index, const float value) override
    {
        if (index < static_cast<uint32_t>(sl_art::P_COUNT)) {
            fState.value[index] = value;
            repaint();
        }
    }

    // ---- drawing ---------------------------------------------------
    void onCairoDisplay(const CairoGraphicsContext& context) override
    {
        cairo_t* const cr = context.handle;
        updateTransform();

        cairo_set_source_rgb(cr, 0.12, 0.12, 0.12);
        cairo_paint(cr);

        cairo_save(cr);
        cairo_translate(cr, fOffX, fOffY);
        cairo_scale(cr, fScale, fScale);
        sl_art::draw(cr, fState);
        if (fTooltip && fHoverParam >= 0)
            drawTooltip(cr);
        cairo_restore(cr);
    }

    // ---- interaction -----------------------------------------------
    bool onMouse(const MouseEvent& ev) override
    {
        if (ev.button != 1)
            return false;

        if (!ev.press) {
            if (fDragging >= 0) {
                editParameter(fDragging, false);
                fDragging = -1;
                return true;
            }
            if (fSelectDrag >= 0) {
                if (!fSelectMoved)
                    stepSelect(fSelectDrag, ev.pos.getX());
                editParameter(fSelectDrag, false);
                fSelectDrag = -1;
                return true;
            }
            return false;
        }

        hideTooltip();
        const sl_art::Control* c = hit(ev.pos.getX(), ev.pos.getY());
        if (c == nullptr) {
            repaint();
            return false;
        }

        if (c->kind == sl_art::kKnob) {
            const bool dbl = c->param == fLastClickParam && ev.time - fLastClickTime < 350;
            fLastClickParam = c->param;
            fLastClickTime = ev.time;
            if (dbl) {
                setValue(c->param, fDefaults[c->param], true);
                return true;
            }
            fDragging = c->param;
            fDragY = ev.pos.getY();
            fDragValue = fState.value[c->param];
            editParameter(c->param, true);
            return true;
        }

        if (c->kind == sl_art::kSelect) {
            fSelectDrag = c->param;
            fSelectY = ev.pos.getY();
            fSelectIndex = currentIndex(c->param);
            if (fSelectIndex < 0)
                fSelectIndex = 0;
            fSelectMoved = false;
            editParameter(c->param, true);
            return true;
        }

        // switches: 2-state toggles, or N-state cycling switches (SOURCE)
        const float cur = fState.value[c->param];
        const int steps = static_cast<int>(c->max) + 1;
        const float next = (steps > 2)
            ? static_cast<float>((static_cast<int>(std::lround(cur)) + 1) % steps)
            : (cur > 0.5f ? 0.0f : 1.0f);
        setValue(c->param, next, true);
        return true;
    }

    bool onMotion(const MotionEvent& ev) override
    {
        if (fSelectDrag >= 0) {
            int count = 0;
            const sl_names::Item* const items = sl_art::selectItems(fSelectDrag, count);
            if (items != nullptr && count > 0) {
                const int steps = static_cast<int>(
                    std::lround((fSelectY - ev.pos.getY()) / 6.0));
                int idx = fSelectIndex + steps;
                if (idx < 0) idx = 0;
                if (idx > count - 1) idx = count - 1;
                if (items[idx].value != static_cast<int>(std::lround(fState.value[fSelectDrag]))) {
                    setValue(fSelectDrag, static_cast<float>(items[idx].value), false);
                    fSelectMoved = true;
                }
            }
            return true;
        }
        if (fDragging < 0) {
            updateHover(ev.pos.getX(), ev.pos.getY());
            return false;
        }
        hideTooltip();
        const sl_art::Control* c = control(fDragging);
        const double span = c->max - c->min;
        // full travel over 200 design units (Shift: 10x finer)
        double pixelsPerTravel = 200.0 * fScale;
        if (ev.mod & kModifierShift)
            pixelsPerTravel *= 10.0;
        const double dv = (fDragY - ev.pos.getY()) / pixelsPerTravel * span;
        setValue(fDragging, static_cast<float>(fDragValue + dv), false);
        return true;
    }

    bool onScroll(const ScrollEvent& ev) override
    {
        const sl_art::Control* c = hit(ev.pos.getX(), ev.pos.getY());
        if (c == nullptr)
            return false;
        if (c->kind == sl_art::kSelect) {
            int count = 0;
            const sl_names::Item* const items = sl_art::selectItems(c->param, count);
            int idx = currentIndex(c->param);
            if (idx < 0)
                idx = 0;
            idx += ev.delta.getY() > 0 ? 1 : -1;
            if (items != nullptr && idx >= 0 && idx < count)
                setValue(c->param, static_cast<float>(items[idx].value), true);
            return true;
        }
        if (c->kind != sl_art::kKnob)
            return false;
        const double step = (c->max - c->min) / ((ev.mod & kModifierShift) ? 500.0 : 50.0);
        setValue(c->param, static_cast<float>(fState.value[c->param] + ev.delta.getY() * step), true);
        return true;
    }

private:
    sl_art::State fState;
    float fDefaults[sl_art::P_COUNT];
    double fScale = 1.0, fOffX = 0.0, fOffY = 0.0;
    int fDragging = -1;
    double fDragY = 0.0;
    double fDragValue = 0.0;
    int fLastClickParam = -1;
    uint fLastClickTime = 0;
    int fSelectDrag = -1;
    double fSelectY = 0.0;
    int fSelectIndex = 0;
    bool fSelectMoved = false;
    int fHoverParam = -1;
    double fHoverX = 0.0, fHoverY = 0.0;
    std::chrono::steady_clock::time_point fHoverSince;
    bool fTooltip = false;

    void hideTooltip()
    {
        if (fTooltip) {
            fTooltip = false;
            repaint();
        }
    }

    void updateHover(const double px, const double py)
    {
        const sl_art::Control* c = hit(px, py);
        const int param = c != nullptr ? c->param : -1;
        if (param != fHoverParam) {
            hideTooltip();
            fHoverParam = param;
            fHoverSince = std::chrono::steady_clock::now();
        }
        if (param >= 0) {
            fHoverX = (px - fOffX) / fScale;
            fHoverY = (py - fOffY) / fScale;
        }
    }

    static std::string noteName(const int note)
    {
        static const char* const names[12] = { "C", "C#", "D", "D#", "E", "F",
                                               "F#", "G", "G#", "A", "A#", "B" };
        char buf[16];
        std::snprintf(buf, sizeof buf, "%s%d", names[((note % 12) + 12) % 12],
                      note / 12 - 1);
        return buf;
    }

    static std::string tooltipText(const int param, const float v)
    {
        const int i = static_cast<int>(std::lround(v));
        char buf[64];
        const auto enumLabel = [&](const sl_names::Item* items, int count) {
            const char* const l = sl_names::lookup(items, count, i);
            return std::string(l != nullptr ? l : "");
        };
        switch (param) {
        case sl_art::P_MODE:       return "Mode: " + enumLabel(sl_names::kMode, sl_names::kModeCount);
        case sl_art::P_BEND:       std::snprintf(buf, sizeof buf, "Bend Range: %d st", i); break;
        case sl_art::P_STEAL:      std::snprintf(buf, sizeof buf, i ? "Note Steal: %d st" : "Note Steal: Off", i); break;
        case sl_art::P_GATE:       std::snprintf(buf, sizeof buf, "Velocity Gate: %d%%", i); break;
        case sl_art::P_LEGATO:     return "Legato: " + enumLabel(sl_names::kLegato, sl_names::kLegatoCount);
        case sl_art::P_GLIDE:      std::snprintf(buf, sizeof buf, "Glide Time: %d ms", i); break;
        case sl_art::P_VIBDEPTH:   std::snprintf(buf, sizeof buf, "Vibrato Depth: %d cents", i); break;
        case sl_art::P_VIBRATE:    std::snprintf(buf, sizeof buf, "Vibrato Rate: %.1f Hz", v); break;
        case sl_art::P_SHIFT:      std::snprintf(buf, sizeof buf, "Chord Shift: %+d st", i); break;
        case sl_art::P_WHEEL:      return "Chord Wheel: " + enumLabel(sl_names::kWheel, sl_names::kWheelCount);
        case sl_art::P_TRANSPOSE:  std::snprintf(buf, sizeof buf, "Transpose: %+d st", i); break;
        case sl_art::P_SCALE:      return "Scale Filter: " + enumLabel(sl_names::kScale, sl_names::kScaleCount);
        case sl_art::P_ROOT:       return "Root: " + noteName(i);
        case sl_art::P_OCTDOWN:
            if (v <= 0.0f) return "Octave Down: Off";
            std::snprintf(buf, sizeof buf, "Octave Down: %.0f%%", v * 100.0f); break;
        case sl_art::P_SPLIT:      return "Split Pitch: " + (i ? noteName(i) : std::string("Off"));
        case sl_art::P_LOWS:       std::snprintf(buf, sizeof buf, "Low Ch: %d", i); break;
        case sl_art::P_HIGHCH:     std::snprintf(buf, sizeof buf, "High Ch: %d", i); break;
        case sl_art::P_BRIGHT_AMT: std::snprintf(buf, sizeof buf, "Brightness Amt: %.0f%%", v * 100.0f); break;
        case sl_art::P_BRIGHT_CC:  std::snprintf(buf, sizeof buf, "Brightness CC#: %d", i); break;
        case sl_art::P_LOUD_AMT:   std::snprintf(buf, sizeof buf, "Loudness Amt: %.0f%%", v * 100.0f); break;
        case sl_art::P_LOUD_CC:    std::snprintf(buf, sizeof buf, "Loudness CC#: %d", i); break;
        case sl_art::P_VELNORM:    std::snprintf(buf, sizeof buf, i ? "Velocity Target: %d" : "Velocity Target: Off", i); break;
        case sl_art::P_OUTCHAN:    std::snprintf(buf, sizeof buf, "Out Ch: %d", i); break;
        case sl_art::P_SOURCE:     return "Source: " + enumLabel(sl_names::kSource, sl_names::kSourceCount);
        case sl_art::P_TRACK_LOW:  return "Lowest Note: " + noteName(i);
        case sl_art::P_TRACK_RES:  std::snprintf(buf, sizeof buf, "Pitch Res: %d", i); break;
        case sl_art::P_TRACK_BRIGHT: std::snprintf(buf, sizeof buf, "Track Tilt: %d", i); break;
        case sl_art::P_TRACK_JITTER: std::snprintf(buf, sizeof buf, "Track Jitter: %d", i); break;
        case sl_art::P_BENDTRIG:   std::snprintf(buf, sizeof buf, "Bend/Trig: %d%%", i); break;
        case sl_art::P_BYPASS:     return std::string("Bypass: ") + (v > 0.5f ? "On" : "Off");
        default:                   return std::string();
        }
        return buf;
    }

    void drawTooltip(cairo_t* cr) const
    {
        const std::string text = tooltipText(fHoverParam, fState.value[fHoverParam]);
        if (text.empty())
            return;
        cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 11.0);
        cairo_text_extents_t e;
        cairo_text_extents(cr, text.c_str(), &e);
        const double pad = 6.0, h = 18.0, w = e.x_advance + 2.0 * pad;
        double x = fHoverX + 16.0;
        double y = fHoverY - h - 12.0;
        if (x + w > sl_art::W - 8.0)
            x = sl_art::W - 8.0 - w;
        if (x < 8.0)
            x = 8.0;
        if (y < 8.0)
            y = fHoverY + 20.0;
        if (y + h > sl_art::H - 8.0)
            y = sl_art::H - 8.0 - h;
        cairo_new_path(cr);
        sl_art::rounded_rect(cr, x, y, w, h, 3, 3, 3, 3);
        cairo_set_source_rgba(cr, 0.06, 0.055, 0.045, 0.95);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 0.78, 0.47, 0.25, 0.9);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);
        cairo_move_to(cr, x + pad, y + h * 0.5 + 4.0);
        cairo_set_source_rgb(cr, 0.90, 0.86, 0.78);
        cairo_show_text(cr, text.c_str());
        cairo_new_path(cr);
    }

    void updateTransform()
    {
        const double w = getWidth(), h = getHeight();
        fScale = std::min(w / sl_art::W, h / sl_art::H);
        fOffX = (w - sl_art::W * fScale) / 2.0;
        fOffY = (h - sl_art::H * fScale) / 2.0;
    }

    int currentIndex(const int param) const
    {
        int count = 0;
        const sl_names::Item* const items = sl_art::selectItems(param, count);
        if (items == nullptr)
            return -1;
        const int v = static_cast<int>(std::lround(fState.value[param]));
        for (int i = 0; i < count; ++i)
            if (items[i].value == v)
                return i;
        return -1;
    }

    // one click step (left arrow area goes back, the rest forward)
    void stepSelect(const int param, const double x)
    {
        const sl_art::Control* const c = control(param);
        if (c == nullptr)
            return;
        int count = 0;
        const sl_names::Item* const items = sl_art::selectItems(param, count);
        if (items == nullptr || count <= 0)
            return;
        const double px = (x - fOffX) / fScale;
        const int dir = px < c->x - c->w * 0.18 ? -1 : 1;
        int idx = currentIndex(param);
        if (idx < 0)
            idx = 0;
        idx += dir;
        if (idx >= 0 && idx < count)
            setValue(param, static_cast<float>(items[idx].value), false);
    }

    static const sl_art::Control* control(const int param)
    {
        for (const sl_art::Control& c : sl_art::kControls)
            if (c.param == param)
                return &c;
        return nullptr;
    }

    const sl_art::Control* hit(const double px, const double py)
    {
        updateTransform();
        const double x = (px - fOffX) / fScale;
        const double y = (py - fOffY) / fScale;
        for (const sl_art::Control& c : sl_art::kControls) {
            if (c.kind == sl_art::kToggle || c.kind == sl_art::kSelect) {
                if (std::abs(x - c.x) <= c.w / 2 && std::abs(y - c.y) <= c.h / 2)
                    return &c;
            } else {
                const double dx = x - c.x, dy = y - c.y;
                if (dx * dx + dy * dy <= c.r * c.r)
                    return &c;
            }
        }
        return nullptr;
    }

    // set locally, notify the host (wrapping in a gesture when needed)
    void setValue(const int param, float value, const bool gesture)
    {
        const sl_art::Control* c = control(param);
        value = std::max(static_cast<float>(c->min), std::min(static_cast<float>(c->max), value));
        if (c->kind != sl_art::kKnob)
            value = static_cast<float>(std::lround(value));
        if (gesture)
            editParameter(param, true);
        fState.value[param] = value;
        setParameterValue(param, value);
        if (gesture)
            editParameter(param, false);
        repaint();
    }

    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(UIStringler)
};

UI* createUI()
{
    return new UIStringler();
}

END_NAMESPACE_DISTRHO
