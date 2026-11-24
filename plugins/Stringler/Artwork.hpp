/*
 * Stringler - pedal artwork, drawn with cairo.
 *
 * Shared by the scalable plugin UI (UIStringler.cpp) and by the
 * modgui renderer (tools/render_modgui.cpp), so the desktop GUI and the
 * modgui look the same. Everything is drawn in a fixed design
 * space of W x H units; callers scale the cairo context.
 *
 * The layout is a dark graphite faceplate with a copper wordmark:
 * three rows of brass knobs (tracking / expression / harmony), a band
 * of small black knobs for the dynamics and routing section, two slide
 * switches (MODE, LEGATO) and a footswitch field. Names and branding
 * are our own.
 *
 * Copyright (C) 2026 Dataforte
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <cairo.h>
#include <cmath>

#include "BrandPath.hpp"
#include "ParamNames.hpp"
#include <cstring>

namespace sl_art {

constexpr double W = 480.0;
constexpr double H = 660.0;

// ---- palette ---------------------------------------------------------
struct RGB { double r, g, b; };
constexpr RGB kBodyTop    { 0.165, 0.158, 0.145 };
constexpr RGB kBodyBottom { 0.095, 0.088, 0.078 };
constexpr RGB kPanel      { 0.050, 0.048, 0.045 };
constexpr RGB kInk        { 0.850, 0.800, 0.720 }; // warm light text
constexpr RGB kDimInk     { 0.520, 0.480, 0.420 };
constexpr RGB kAccent     { 0.780, 0.470, 0.250 }; // copper
constexpr RGB kBrassLight { 0.820, 0.600, 0.280 };
constexpr RGB kBrassDark  { 0.360, 0.220, 0.100 };

// ---- controls --------------------------------------------------------
enum Kind { kKnob, kToggle, kFootswitch, kSelect };

struct Control {
    int param;
    Kind kind;
    double x, y, r;   // centre and hit radius
    double w, h;      // hit box for toggles
    double min, max;
};

// parameter indices (mirrors DistrhoPluginInfo.h)
enum {
    P_MODE, P_BEND, P_STEAL, P_GATE, P_LEGATO, P_GLIDE,
    P_VIBDEPTH, P_VIBRATE, P_SHIFT, P_WHEEL, P_TRANSPOSE,
    P_SCALE, P_ROOT, P_OCTDOWN, P_SPLIT, P_LOWS, P_HIGHCH,
    P_BRIGHT_AMT, P_BRIGHT_CC, P_LOUD_AMT, P_LOUD_CC,
    P_VELNORM, P_OUTCHAN, P_SOURCE,
    P_TRACK_LOW, P_TRACK_RES, P_TRACK_BRIGHT, P_TRACK_JITTER, P_BENDTRIG,
    P_BYPASS, P_COUNT
};

// Positions are snapped so that every modgui element (centre minus half
// its film box) lands on a whole CSS pixel (1.25 design units): browsers
// place backgrounds at fractional offsets with sub-pixel blur otherwise.
// tools/render_modgui.cpp refuses to generate fractional positions.
constexpr double MAIN_RING = 31.0, MAIN_KNOB = 22.0;
constexpr double SMALL_RING = 17.0, SMALL_KNOB = 12.0;
constexpr double MX[4] = { 55.0, 175.0, 295.0, 415.0 };
constexpr double MY[3] = { 100.0, 200.0, 300.0 };
constexpr double SX1[5] = { 50.0, 140.0, 240.0, 330.0, 420.0 };
constexpr double SY1 = 380.0;
constexpr double SX2[4] = { 60.0, 180.0, 300.0, 420.0 };
constexpr double SY2 = 462.5;
// SW_Y snapped so the switch film-box edge is a whole CSS pixel (x0.8)
constexpr double SW_X1 = 55.0,  SW_X2 = 425.0, SW_Y = 43.75;
constexpr double TR_X[5] = { 50.0, 145.0, 240.0, 335.0, 430.0 };
constexpr double CH_W = 62.5, CH_H = 25.0;   // channel selector badge
constexpr double TR_Y = 555.0;
constexpr double SRC_X = 85.0, SRC_Y = 620.0;
constexpr double FOOT_X = 240.0, FOOT_Y = 620.0;
constexpr double LED_X  = 170.0, LED_Y  = 620.0;

constexpr Control kControls[] = {
    { P_MODE,      kToggle,     SW_X1,  SW_Y, 0, 30, 16, 0,   1   },
    { P_BEND,      kKnob,       MX[0],  MY[0], MAIN_RING, 0, 0,  0,   48 },
    { P_STEAL,     kKnob,       MX[1],  MY[0], MAIN_RING, 0, 0,  0,   12 },
    { P_GATE,      kKnob,       MX[2],  MY[0], MAIN_RING, 0, 0,  0,   1  },
    { P_GLIDE,     kKnob,       MX[3],  MY[0], MAIN_RING, 0, 0,  0,  100 },
    { P_LEGATO,    kToggle,     SW_X2,  SW_Y, 0, 30, 16, 0,   1   },
    { P_VIBDEPTH,  kKnob,       MX[0],  MY[1], MAIN_RING, 0, 0,  0,  200 },
    { P_VIBRATE,   kKnob,       MX[1],  MY[1], MAIN_RING, 0, 0,  0,   10 },
    { P_SHIFT,     kKnob,       MX[2],  MY[1], MAIN_RING, 0, 0, -12,  12 },
    { P_WHEEL,     kSelect,     MX[3],  MY[1], 0, 100, 25,  0,    3 },
    { P_TRANSPOSE, kKnob,       MX[0],  MY[2], MAIN_RING, 0, 0, -24,  24 },
    { P_SCALE,     kSelect,     MX[1],  MY[2], 0, 100, 25,  0,   17 },
    { P_ROOT,      kSelect,     MX[2],  MY[2], 0, 100, 25,  0,   11 },
    { P_OCTDOWN,   kKnob,       MX[3],  MY[2], MAIN_RING, 0, 0,  0,    2 },
    { P_OUTCHAN,   kSelect,     SX2[3], SY2, 0, CH_W, CH_H, 1,   16 },
    { P_BRIGHT_AMT,kKnob,       SX1[0], SY1,  SMALL_RING, 0, 0, 0,    2 },
    { P_BRIGHT_CC, kKnob,       SX1[1], SY1,  SMALL_RING, 0, 0, 0,  127 },
    { P_LOUD_AMT,  kKnob,       SX1[2], SY1,  SMALL_RING, 0, 0, 0,    2 },
    { P_LOUD_CC,   kKnob,       SX1[3], SY1,  SMALL_RING, 0, 0, 0,  127 },
    { P_VELNORM,   kKnob,       SX1[4], SY1,  SMALL_RING, 0, 0, 0,  127 },
    { P_LOWS,      kSelect,     SX2[0], SY2, 0, CH_W, CH_H, 1,   16 },
    { P_SPLIT,     kSelect,     SX2[1], SY2, 0, CH_W, CH_H, 0,   64 },
    { P_HIGHCH,    kSelect,     SX2[2], SY2, 0, CH_W, CH_H, 1,   16 },

    { P_TRACK_LOW, kKnob,       TR_X[0], TR_Y, SMALL_RING, 0, 0, 33, 60 },
    { P_TRACK_RES, kKnob,       TR_X[1], TR_Y, SMALL_RING, 0, 0, 0,   3 },
    { P_TRACK_BRIGHT, kKnob,    TR_X[2], TR_Y, SMALL_RING, 0, 0, 0,   4 },
    { P_TRACK_JITTER, kKnob,    TR_X[3], TR_Y, SMALL_RING, 0, 0, 0,   3 },
    { P_BENDTRIG,  kKnob,       TR_X[4], TR_Y, SMALL_RING, 0, 0, 0, 100 },
    { P_SOURCE,    kToggle,     SRC_X,  SRC_Y, 0, 30, 16, 0,   2   },
    { P_BYPASS,    kFootswitch, FOOT_X, FOOT_Y, 24.0, 0, 0, 0,    1 },
};
constexpr int kNumControls = sizeof(kControls) / sizeof(kControls[0]);

struct State {
    float value[P_COUNT] = {
        0.0f, 12.0f, 1.0f, 0.1f, 1.0f, 30.0f,
        0.0f, 5.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 50.0f, 1.0f, 1.0f,
        0.0f, 1.0f, 0.0f, 7.0f,
        0.0f, 1.0f, 2.0f,
        40.0f, 0.0f, 2.0f, 0.0f, 50.0f,
        0.0f
    };
};

// ---- helpers ---------------------------------------------------------
inline void set(cairo_t* cr, const RGB& c, double a = 1.0)
{
    cairo_set_source_rgba(cr, c.r, c.g, c.b, a);
}

inline void rounded_rect(cairo_t* cr, double x, double y, double w, double h,
                         double rtl, double rtr, double rbr, double rbl)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - rtr, y + rtr, rtr, -M_PI / 2, 0);
    cairo_arc(cr, x + w - rbr, y + h - rbr, rbr, 0, M_PI / 2);
    cairo_arc(cr, x + rbl, y + h - rbl, rbl, M_PI / 2, M_PI);
    cairo_arc(cr, x + rtl, y + rtl, rtl, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

enum Align { kLeft, kCenter, kRight };

// Draw text with its baseline at y. If maxw > 0 the text is squeezed
// horizontally to fit; if fitw > 0 it is stretched/squeezed to exactly fitw.
inline void text(cairo_t* cr, const char* s, double x, double y, double size,
                 const RGB& c, Align align = kCenter, bool bold = true,
                 const char* family = "DejaVu Sans Condensed",
                 double maxw = 0.0, double fitw = 0.0,
                 cairo_font_slant_t slant = CAIRO_FONT_SLANT_NORMAL)
{
    cairo_save(cr);
    cairo_select_font_face(cr, family, slant,
                           bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, size);
    cairo_text_extents_t e;
    cairo_text_extents(cr, s, &e);
    double sx = 1.0;
    if (fitw > 0.0 && e.x_advance > 0.0)
        sx = fitw / e.x_advance;
    else if (maxw > 0.0 && e.x_advance > maxw)
        sx = maxw / e.x_advance;
    const double w = e.x_advance * sx;
    double x0 = x;
    if (align == kCenter) x0 = x - w / 2;
    else if (align == kRight) x0 = x - w;
    cairo_translate(cr, x0, y);
    cairo_scale(cr, sx, 1.0);
    cairo_move_to(cr, 0, 0);
    set(cr, c);
    cairo_show_text(cr, s);
    cairo_restore(cr);
    cairo_new_path(cr);
}

// 0..1 -> angle (radians, cairo convention), 7 o'clock .. 5 o'clock
inline double knob_angle(double norm)
{
    const double deg = -150.0 + 300.0 * norm;      // 0 = 12 o'clock
    return (deg - 90.0) * M_PI / 180.0;
}

inline cairo_pattern_t* body_gradient()
{
    cairo_pattern_t* g = cairo_pattern_create_linear(0, 0, 0, H);
    cairo_pattern_add_color_stop_rgb(g, 0, kBodyTop.r, kBodyTop.g, kBodyTop.b);
    cairo_pattern_add_color_stop_rgb(g, 1, kBodyBottom.r, kBodyBottom.g, kBodyBottom.b);
    return g;
}

// ---- parts -----------------------------------------------------------
inline void screw(cairo_t* cr, double x, double y, double r, bool dark = false)
{
    cairo_new_path(cr);
    cairo_pattern_t* g = cairo_pattern_create_radial(x - r * 0.4, y - r * 0.4, r * 0.1, x, y, r);
    if (dark) {
        cairo_pattern_add_color_stop_rgb(g, 0, 0.55, 0.55, 0.55);
        cairo_pattern_add_color_stop_rgb(g, 1, 0.20, 0.20, 0.20);
    } else {
        cairo_pattern_add_color_stop_rgb(g, 0, 0.92, 0.92, 0.90);
        cairo_pattern_add_color_stop_rgb(g, 1, 0.45, 0.45, 0.43);
    }
    cairo_arc(cr, x, y, r, 0, 2 * M_PI);
    cairo_set_source(cr, g);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(g);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
    cairo_set_line_width(cr, 0.6);
    cairo_stroke(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.7);
    cairo_set_line_width(cr, r * 0.28);
    cairo_move_to(cr, x - r * 0.6, y - r * 0.15); cairo_line_to(cr, x + r * 0.6, y + r * 0.15);
    cairo_move_to(cr, x - r * 0.15, y + r * 0.6); cairo_line_to(cr, x + r * 0.15, y - r * 0.6);
    cairo_stroke(cr);
}

// Brass pointer knob for the main rows.
inline void brass_knob(cairo_t* cr, double cx, double cy, double r, double norm)
{
    const double a = knob_angle(norm);
    cairo_new_path(cr);

    // shadow
    cairo_pattern_t* sh = cairo_pattern_create_radial(cx + r * 0.12, cy + r * 0.18, r * 0.8,
                                                       cx + r * 0.12, cy + r * 0.18, r * 1.25);
    cairo_pattern_add_color_stop_rgba(sh, 0, 0, 0, 0, 0.45);
    cairo_pattern_add_color_stop_rgba(sh, 1, 0, 0, 0, 0.0);
    cairo_arc(cr, cx + r * 0.12, cy + r * 0.18, r * 1.25, 0, 2 * M_PI);
    cairo_set_source(cr, sh);
    cairo_fill(cr);
    cairo_pattern_destroy(sh);

    // bezel ring
    cairo_pattern_t* bz = cairo_pattern_create_linear(cx - r, cy - r, cx + r, cy + r);
    cairo_pattern_add_color_stop_rgb(bz, 0, 0.42, 0.28, 0.12);
    cairo_pattern_add_color_stop_rgb(bz, 0.5, 0.22, 0.14, 0.06);
    cairo_pattern_add_color_stop_rgb(bz, 1, 0.10, 0.07, 0.03);
    cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
    cairo_set_source(cr, bz);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(bz);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.9);
    cairo_set_line_width(cr, r * 0.03);
    cairo_stroke(cr);

    // pointer fin on the bezel
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_rotate(cr, a);
    cairo_move_to(cr, r * 1.06, 0);
    cairo_line_to(cr, r * 0.80, -r * 0.13);
    cairo_line_to(cr, r * 0.80,  r * 0.13);
    cairo_close_path(cr);
    set(cr, kAccent);
    cairo_fill(cr);
    cairo_restore(cr);

    // top face
    cairo_pattern_t* f = cairo_pattern_create_radial(cx - r * 0.3, cy - r * 0.35, r * 0.05,
                                                      cx, cy, r * 0.8);
    cairo_pattern_add_color_stop_rgb(f, 0, kBrassLight.r, kBrassLight.g, kBrassLight.b);
    cairo_pattern_add_color_stop_rgb(f, 0.6, 0.55, 0.38, 0.17);
    cairo_pattern_add_color_stop_rgb(f, 1, kBrassDark.r, kBrassDark.g, kBrassDark.b);
    cairo_arc(cr, cx, cy, r * 0.78, 0, 2 * M_PI);
    cairo_set_source(cr, f);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(f);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.8);
    cairo_set_line_width(cr, r * 0.04);
    cairo_stroke(cr);

    // pointer line
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_width(cr, r * 0.10);
    cairo_set_source_rgb(cr, 0.92, 0.88, 0.78);
    cairo_move_to(cr, cx + r * 0.22 * cos(a), cy + r * 0.22 * sin(a));
    cairo_line_to(cr, cx + r * 0.70 * cos(a), cy + r * 0.70 * sin(a));
    cairo_stroke(cr);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
}

// Black knurled pointer knob for the small rows, lit from top left.
inline void black_knob(cairo_t* cr, double cx, double cy, double r, double norm)
{
    const double a = knob_angle(norm);
    cairo_new_path(cr);

    // shadow
    cairo_pattern_t* sh = cairo_pattern_create_radial(cx + r * 0.12, cy + r * 0.18, r * 0.8,
                                                       cx + r * 0.12, cy + r * 0.18, r * 1.25);
    cairo_pattern_add_color_stop_rgba(sh, 0, 0, 0, 0, 0.45);
    cairo_pattern_add_color_stop_rgba(sh, 1, 0, 0, 0, 0.0);
    cairo_arc(cr, cx + r * 0.12, cy + r * 0.18, r * 1.25, 0, 2 * M_PI);
    cairo_set_source(cr, sh);
    cairo_fill(cr);
    cairo_pattern_destroy(sh);

    // knurled skirt (teeth rotate with the knob)
    const int teeth = 36;
    cairo_new_path(cr);
    for (int i = 0; i < teeth * 2; ++i) {
        const double t = a + i * M_PI / teeth;
        const double rr = (i % 2) ? r * 0.94 : r;
        if (i == 0) cairo_move_to(cr, cx + rr * cos(t), cy + rr * sin(t));
        else        cairo_line_to(cr, cx + rr * cos(t), cy + rr * sin(t));
    }
    cairo_close_path(cr);
    cairo_pattern_t* sk = cairo_pattern_create_linear(cx - r, cy - r, cx + r, cy + r);
    cairo_pattern_add_color_stop_rgb(sk, 0, 0.30, 0.30, 0.30);
    cairo_pattern_add_color_stop_rgb(sk, 0.5, 0.10, 0.10, 0.10);
    cairo_pattern_add_color_stop_rgb(sk, 1, 0.02, 0.02, 0.02);
    cairo_set_source(cr, sk);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(sk);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.9);
    cairo_set_line_width(cr, r * 0.03);
    cairo_stroke(cr);

    // pointer fin on the skirt
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_rotate(cr, a);
    cairo_move_to(cr, r * 1.06, 0);
    cairo_line_to(cr, r * 0.80, -r * 0.13);
    cairo_line_to(cr, r * 0.80,  r * 0.13);
    cairo_close_path(cr);
    set(cr, kAccent);
    cairo_fill(cr);
    cairo_restore(cr);

    // top face
    cairo_pattern_t* f = cairo_pattern_create_radial(cx - r * 0.3, cy - r * 0.35, r * 0.05,
                                                      cx, cy, r * 0.8);
    cairo_pattern_add_color_stop_rgb(f, 0, 0.36, 0.36, 0.36);
    cairo_pattern_add_color_stop_rgb(f, 0.6, 0.13, 0.13, 0.13);
    cairo_pattern_add_color_stop_rgb(f, 1, 0.06, 0.06, 0.06);
    cairo_arc(cr, cx, cy, r * 0.78, 0, 2 * M_PI);
    cairo_set_source(cr, f);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(f);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.8);
    cairo_set_line_width(cr, r * 0.04);
    cairo_stroke(cr);

    // pointer line
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_width(cr, r * 0.10);
    cairo_set_source_rgb(cr, 0.86, 0.86, 0.84);
    cairo_move_to(cr, cx + r * 0.22 * cos(a), cy + r * 0.22 * sin(a));
    cairo_line_to(cr, cx + r * 0.70 * cos(a), cy + r * 0.70 * sin(a));
    cairo_stroke(cr);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
}

// Miniature slide switch, thumb left (0) or right (1).
inline void slide_switch(cairo_t* cr, double cx, double cy, int pos, int steps = 2)
{
    const double w = 28, h = 11;
    cairo_new_path(cr);
    rounded_rect(cr, cx - w / 2, cy - h / 2, w, h, 3, 3, 3, 3);
    cairo_set_source_rgb(cr, 0.12, 0.12, 0.12);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.35);
    cairo_set_line_width(cr, 0.8);
    cairo_stroke(cr);
    const double tw = 12, th = 9;
    const double t = steps > 1 ? static_cast<double>(pos) / (steps - 1) : 0.0;
    const double tx = cx - w / 2 + 1 + t * (w - tw - 2);
    cairo_pattern_t* g = cairo_pattern_create_linear(tx, cy - th / 2, tx, cy + th / 2);
    cairo_pattern_add_color_stop_rgb(g, 0, 0.95, 0.95, 0.93);
    cairo_pattern_add_color_stop_rgb(g, 1, 0.55, 0.55, 0.53);
    rounded_rect(cr, tx, cy - th / 2, tw, th, 2, 2, 2, 2);
    cairo_set_source(cr, g);
    cairo_fill(cr);
    cairo_pattern_destroy(g);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
    cairo_set_line_width(cr, 0.8);
    for (int i = 0; i < 3; ++i) {
        const double lx = tx + 3.5 + i * 2.5;
        cairo_move_to(cr, lx, cy - th / 2 + 2);
        cairo_line_to(cr, lx, cy + th / 2 - 2);
    }
    cairo_stroke(cr);
}

inline void footswitch(cairo_t* cr, double cx, double cy)
{
    // hex nut
    const double rn = 19;
    cairo_new_path(cr);
    for (int i = 0; i < 6; ++i) {
        const double t = M_PI / 6 + i * M_PI / 3;
        if (i == 0) cairo_move_to(cr, cx + rn * cos(t), cy + rn * sin(t));
        else        cairo_line_to(cr, cx + rn * cos(t), cy + rn * sin(t));
    }
    cairo_close_path(cr);
    cairo_pattern_t* g = cairo_pattern_create_linear(cx - rn, cy - rn, cx + rn, cy + rn);
    cairo_pattern_add_color_stop_rgb(g, 0, 0.92, 0.92, 0.90);
    cairo_pattern_add_color_stop_rgb(g, 1, 0.38, 0.38, 0.37);
    cairo_set_source(cr, g);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(g);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
    cairo_set_line_width(cr, 0.8);
    cairo_stroke(cr);
    // threaded collar
    cairo_arc(cr, cx, cy, 13.5, 0, 2 * M_PI);
    cairo_set_source_rgb(cr, 0.55, 0.55, 0.54);
    cairo_fill(cr);
    // plunger
    cairo_pattern_t* p = cairo_pattern_create_radial(cx - 3, cy - 4, 1, cx, cy, 10);
    cairo_pattern_add_color_stop_rgb(p, 0, 1.0, 1.0, 1.0);
    cairo_pattern_add_color_stop_rgb(p, 0.5, 0.80, 0.80, 0.79);
    cairo_pattern_add_color_stop_rgb(p, 1, 0.45, 0.45, 0.44);
    cairo_arc(cr, cx, cy, 9.5, 0, 2 * M_PI);
    cairo_set_source(cr, p);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(p);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.5);
    cairo_stroke(cr);
}

inline void led(cairo_t* cr, double cx, double cy, bool lit)
{
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, 6.0, 0, 2 * M_PI);
    cairo_set_source_rgb(cr, 0.25, 0.25, 0.25);
    cairo_fill(cr);
    if (lit) {
        cairo_pattern_t* glow = cairo_pattern_create_radial(cx, cy, 2, cx, cy, 14);
        cairo_pattern_add_color_stop_rgba(glow, 0, 1.0, 0.15, 0.1, 0.55);
        cairo_pattern_add_color_stop_rgba(glow, 1, 1.0, 0.15, 0.1, 0.0);
        cairo_arc(cr, cx, cy, 14, 0, 2 * M_PI);
        cairo_set_source(cr, glow);
        cairo_fill(cr);
        cairo_pattern_destroy(glow);
    }
    cairo_pattern_t* g = cairo_pattern_create_radial(cx - 1.3, cy - 1.5, 0.5, cx, cy, 4.5);
    if (lit) {
        cairo_pattern_add_color_stop_rgb(g, 0, 1.0, 0.85, 0.80);
        cairo_pattern_add_color_stop_rgb(g, 0.4, 1.0, 0.18, 0.10);
        cairo_pattern_add_color_stop_rgb(g, 1, 0.65, 0.03, 0.02);
    } else {
        cairo_pattern_add_color_stop_rgb(g, 0, 0.55, 0.25, 0.22);
        cairo_pattern_add_color_stop_rgb(g, 1, 0.22, 0.03, 0.02);
    }
    cairo_arc(cr, cx, cy, 4.5, 0, 2 * M_PI);
    cairo_set_source(cr, g);
    cairo_fill(cr);
    cairo_pattern_destroy(g);
}

// Wordmark: "STRINGLER" in copper, with the caption "MIDI EXPRESSION".
inline void logo(cairo_t* cr)
{
    cairo_save(cr);
    cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 30.0);
    cairo_text_extents_t e;
    cairo_text_extents(cr, "STRINGLER", &e);
    const double sx = 178.0 / e.width;
    cairo_translate(cr, W / 2 - (e.x_bearing + e.width / 2) * sx, 48.0);
    cairo_scale(cr, sx, 0.92);
    cairo_move_to(cr, 0, 0);
    cairo_text_path(cr, "STRINGLER");
    cairo_restore(cr);
    set(cr, kAccent);
    cairo_fill(cr);

    text(cr, "MIDI Expression", W / 2, 64, 10.0, kDimInk);
}

// Mini chord symbol: three dots spaced by the chord intervals
// (root, third, fifth) in semitones.
inline void chord_icon(cairo_t* cr, double cx, double cy, int wheel)
{
    static const int iv[4][3] = { { 0, 0, 0 }, { 0, 4, 7 }, { 0, 3, 7 }, { 0, 3, 6 } };
    if (wheel < 1 || wheel > 3)
        return;
    set(cr, kInk, 0.85);
    for (int i = 0; i < 3; ++i) {
        const double y = cy + 7.0 - iv[wheel][i] * 2.0;
        cairo_arc(cr, cx, y, i == 0 ? 2.0 : 1.6, 0, 2 * M_PI);
        cairo_fill(cr);
    }
}

// Selector badge for named values: dark inset field, value text, caret.
// With an icon the text starts further right (used for the chord wheel).
inline void select_badge(cairo_t* cr, double cx, double cy, double w, double h,
                         const char* value, bool withIcon = false, int wheel = 0)
{
    const double x = cx - w / 2, y = cy - h / 2;
    cairo_new_path(cr);
    rounded_rect(cr, x, y, w, h, 5, 5, 5, 5);
    cairo_pattern_t* g = cairo_pattern_create_linear(0, y, 0, y + h);
    cairo_pattern_add_color_stop_rgb(g, 0, 0.085, 0.080, 0.072);
    cairo_pattern_add_color_stop_rgb(g, 1, 0.045, 0.042, 0.038);
    cairo_set_source(cr, g);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(g);
    set(cr, kAccent, 0.55);
    cairo_set_line_width(cr, 1.2);
    cairo_stroke(cr);
    if (withIcon)
        chord_icon(cr, x + 13, cy, wheel);
    if (value != nullptr && value[0] != 0)
        text(cr, value, x + (withIcon ? 26 : 8), cy + 3.2, 9.0, kInk, kLeft, true,
             "DejaVu Sans Condensed", w - (withIcon ? 44 : 26));
    // caret
    cairo_new_path(cr);
    cairo_move_to(cr, x + w - 14, cy - 2.5);
    cairo_line_to(cr, x + w - 8,  cy - 2.5);
    cairo_line_to(cr, x + w - 11, cy + 2.5);
    cairo_close_path(cr);
    set(cr, kInk, 0.75);
    cairo_fill(cr);
}

inline Kind kindOf(const int param)
{
    for (const Control& c : kControls)
        if (c.param == param)
            return c.kind;
    return kKnob;
}

inline const sl_names::Item* selectItems(const int param, int& count)
{
    using namespace sl_names;
    switch (param) {
    case P_WHEEL:  count = kWheelCount;   return kWheel;
    case P_SCALE:  count = kScaleCount;   return kScale;
    case P_ROOT:   count = kRootCount;    return kRoot;
    case P_SPLIT:  count = kSplitCount;   return kSplit;
    case P_LOWS:
    case P_HIGHCH:
    case P_OUTCHAN: count = kChannelCount; return kChannel;
    default:       count = 0;             return nullptr;
    }
}

inline const char* selectLabel(const int param, const float value)
{
    int count = 0;
    const sl_names::Item* const items = selectItems(param, count);
    if (items == nullptr)
        return nullptr;
    return sl_names::lookup(items, count, static_cast<int>(std::lround(value)));
}

// MIDI-style note symbols: one note = mono, a three-note cluster = poly.
inline void mode_icon(cairo_t* cr, double cx, double cy, bool chord)
{
    set(cr, kInk, 0.8);
    const double headR = 2.1;
    const int notes = chord ? 3 : 1;
    for (int i = 0; i < notes; ++i) {
        const double hx = cx + (i - (notes - 1) * 0.5) * 5.0;
        const double hy = cy + 3.2;
        cairo_save(cr);
        cairo_translate(cr, hx, hy);
        cairo_scale(cr, 1.0, 0.8);
        cairo_arc(cr, 0, 0, headR, 0, 2 * M_PI);
        cairo_fill(cr);
        cairo_restore(cr);
        cairo_set_line_width(cr, 0.9);
        cairo_move_to(cr, hx + headR * 0.95, hy);
        cairo_line_to(cr, hx + headR * 0.95, hy - 7.4);
        cairo_stroke(cr);
    }
}

// audio waveform (signal) icon
inline void icon_wave(cairo_t* cr, double cx, double cy, double s)
{
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_scale(cr, s, s);
    set(cr, kInk, 0.8);
    cairo_set_line_width(cr, 1.2);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_move_to(cr, -8.0, 0.0);
    cairo_line_to(cr, -5.0, 0.0);
    cairo_curve_to(cr, -3.4, -6.0, -1.6, -6.0, 0.0, 0.0);
    cairo_curve_to(cr, 1.6, 6.0, 3.4, 6.0, 5.0, 0.0);
    cairo_line_to(cr, 8.0, 0.0);
    cairo_stroke(cr);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
    cairo_restore(cr);
}

// five-pin DIN connector (MIDI) icon
inline void icon_din(cairo_t* cr, double cx, double cy, double s)
{
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_scale(cr, s, s);
    set(cr, kInk, 0.8);
    cairo_set_line_width(cr, 1.1);
    cairo_arc(cr, 0.0, -1.0, 6.2, 0.10 * M_PI, 0.90 * M_PI);
    cairo_stroke(cr);
    for (int i = 0; i < 5; ++i) {
        const double a = (0.18 + 0.16 * i) * M_PI;
        cairo_arc(cr, 3.6 * cos(a), -1.0 + 3.6 * sin(a), 0.95, 0, 2 * M_PI);
        cairo_fill(cr);
    }
    cairo_restore(cr);
}

// two notes: detached (off) or slurred (legato on)
inline void icon_legato(cairo_t* cr, double cx, double cy, bool on)
{
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    set(cr, kInk, 0.8);
    const double hs[2] = { -4.6, 3.4 };
    for (int i = 0; i < 2; ++i) {
        cairo_save(cr);
        cairo_translate(cr, hs[i], 3.6);
        cairo_scale(cr, 1.0, 0.8);
        cairo_arc(cr, 0, 0, 2.0, 0, 2 * M_PI);
        cairo_fill(cr);
        cairo_restore(cr);
        cairo_set_line_width(cr, 0.9);
        cairo_move_to(cr, hs[i] + 1.9, 3.6);
        cairo_line_to(cr, hs[i] + 1.9, -4.4);
        cairo_stroke(cr);
    }
    if (on) {
        // slur joining the two notes
        cairo_set_line_width(cr, 1.1);
        cairo_move_to(cr, hs[0] + 1.9, -4.4);
        cairo_curve_to(cr, hs[0] + 3.6, -8.2, hs[1] + 0.2, -8.2, hs[1] + 1.9, -4.4);
        cairo_stroke(cr);
    } else {
        // staccato dots
        for (int i = 0; i < 2; ++i) {
            cairo_arc(cr, hs[i] + 1.9, -6.4, 0.8, 0, 2 * M_PI);
            cairo_fill(cr);
        }
    }
    cairo_restore(cr);
}

// subtle box around a group of related controls, with a caption above it
inline void group_frame(cairo_t* cr, double x1, double y1, double x2, double y2,
                        const char* caption)
{
    cairo_new_path(cr);
    rounded_rect(cr, x1, y1, x2 - x1, y2 - y1, 9, 9, 9, 9);
    set(cr, kInk, 0.14);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);
    text(cr, caption, (x1 + x2) / 2, y1 - 6.5, 7.0, kDimInk, kCenter, true,
         "DejaVu Sans Condensed", 0.0);
}

// ---- layers ----------------------------------------------------------
enum Layer {
    kLayerBody     = 1 << 0,   // box, print, rings, panel, footswitch
    kLayerKnobs    = 1 << 1,   // the twelve main brass knobs
    kLayerSmall    = 1 << 2,   // the nine small black knobs
    kLayerSwitches = 1 << 3,   // MODE / LEGATO slide switches
    kLayerLed      = 1 << 4,
    kLayerAll      = 0xff
};

inline double norm(const State& s, int p)
{
    for (const Control& c : kControls)
        if (c.param == p)
            return (s.value[p] - c.min) / (c.max - c.min);
    return 0.0;
}

inline void draw(cairo_t* cr, const State& s, unsigned layers = kLayerAll)
{
    if (layers & kLayerBody) {
        // box
        cairo_new_path(cr);
        cairo_pattern_t* g = body_gradient();
        rounded_rect(cr, 1, 1, W - 2, H - 2, 9, 9, 9, 9);
        cairo_set_source(cr, g);
        cairo_fill_preserve(cr);
        cairo_pattern_destroy(g);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.55);
        cairo_set_line_width(cr, 1.5);
        cairo_stroke(cr);
        // copper pinstripe inset
        rounded_rect(cr, 6, 6, W - 12, H - 12, 7, 7, 7, 7);
        set(cr, kAccent, 0.5);
        cairo_set_line_width(cr, 1.2);
        cairo_stroke(cr);

        // jack legends along the top edge
        text(cr, "Out", 40, 16, 9.0, kDimInk);
        text(cr, "In", 440, 16, 9.0, kDimInk);
        mode_icon(cr, 30, 43.75, false);   // mono: one note, left of the switch
        mode_icon(cr, 80, 43.75, true);    // poly: a chord, right of it
        icon_legato(cr, 400, 43.75, false);   // detached
        icon_legato(cr, 450, 43.75, true);    // slurred world

        // main knob rings and legends
        const char* names[3][4] = {
            { "Bend", "Steal", "Gate", "Glide" },
            { "Vib Depth", "Vib Rate", "Shift", "Chords" },
            { "Transpose", "Scale", "Root", "Oct Down" },
        };
        const int mainParams[3][4] = {
            { P_BEND, P_STEAL, P_GATE, P_GLIDE },
            { P_VIBDEPTH, P_VIBRATE, P_SHIFT, P_WHEEL },
            { P_TRANSPOSE, P_SCALE, P_ROOT, P_OCTDOWN },
        };
        for (int ry = 0; ry < 3; ++ry) {
            for (int cx = 0; cx < 4; ++cx) {
                const bool select = kindOf(mainParams[ry][cx]) == kSelect;
                if (!select) {
                    cairo_new_path(cr);
                    cairo_arc(cr, MX[cx], MY[ry], MAIN_RING, 0, 2 * M_PI);
                    set(cr, kInk, 0.55);
                    cairo_set_line_width(cr, 1.4);
                    cairo_stroke(cr);
                }
                text(cr, names[ry][cx], MX[cx], MY[ry] + MAIN_RING + 13, 9.5, kInk,
                     kCenter, true, "DejaVu Sans Condensed", 104.0);
            }
        }

        // small knob rings and legends
        const char* s1[5] = { "Bright", "Bright CC#", "Loud", "Loud CC#", "Vel Target" };
        for (int i = 0; i < 5; ++i) {
            cairo_new_path(cr);
            cairo_arc(cr, SX1[i], SY1, SMALL_RING, 0, 2 * M_PI);
            set(cr, kInk, 0.45);
            cairo_set_line_width(cr, 1.1);
            cairo_stroke(cr);
            text(cr, s1[i], SX1[i], SY1 + SMALL_RING + 9, 8.0, kInk,
                 kCenter, true, "DejaVu Sans Condensed", 82.0);
        }
        // tracker row (recovered settings)
        const char* s3[5] = { "Lowest", "Pitch Res", "Tilt", "Jitter", "Bend/Trig" };
        for (int i = 0; i < 5; ++i) {
            cairo_new_path(cr);
            cairo_arc(cr, TR_X[i], TR_Y, SMALL_RING, 0, 2 * M_PI);
            set(cr, kInk, 0.45);
            cairo_set_line_width(cr, 1.1);
            cairo_stroke(cr);
            text(cr, s3[i], TR_X[i], TR_Y + SMALL_RING + 9, 8.0, kInk,
                 kCenter, true, "DejaVu Sans Condensed", 96.0);
        }

        const char* s2[4] = { "Low Ch", "Split", "High Ch", "Out Ch" };
        const int routingParams[4] = { P_LOWS, P_SPLIT, P_HIGHCH, P_OUTCHAN };
        for (int i = 0; i < 4; ++i) {
            const int param = routingParams[i];
            if (kindOf(param) != kSelect) {
                cairo_new_path(cr);
                cairo_arc(cr, SX2[i], SY2, SMALL_RING, 0, 2 * M_PI);
                set(cr, kInk, 0.45);
                cairo_set_line_width(cr, 1.1);
                cairo_stroke(cr);
            }
            text(cr, s2[i], SX2[i], SY2 + SMALL_RING + 9, 8.0, kInk,
                 kCenter, true, "DejaVu Sans Condensed", 82.0);
        }

        // related controls are boxed together
        group_frame(cr, 22, 168, 208, 232, "Vibrato");
        group_frame(cr, 120, 278, 350, 322, "Scale Filter");
        group_frame(cr, 24, 440, 456, 482, "Routing");

        screw(cr, 21, 250, 4.5);
        screw(cr, W - 21, 250, 4.5);

        logo(cr);

        // footswitch field with rounded bottom corners
        cairo_new_path(cr);
        rounded_rect(cr, 19, 590, W - 38, 60, 2, 2, 26, 26);
        cairo_pattern_t* pg = cairo_pattern_create_linear(0, 590, 0, 650);
        cairo_pattern_add_color_stop_rgb(pg, 0, 0.07, 0.068, 0.063);
        cairo_pattern_add_color_stop_rgb(pg, 1, kPanel.r, kPanel.g, kPanel.b);
        cairo_set_source(cr, pg);
        cairo_fill(cr);
        cairo_pattern_destroy(pg);

        screw(cr, 32, 620, 4.0, true);
        screw(cr, W - 32, 620, 4.0, true);

        footswitch(cr, FOOT_X, FOOT_Y);

        // maker line: Bitcount Grid Double wordmark baked by tools/gen_brand.py,
        // so the cairo UI and the modgui match without the font installed
        // SOURCE position icons, on top of the footswitch panel
        icon_wave(cr, SRC_X - 24, SRC_Y + 16, 0.72);          // audio
        icon_din(cr, SRC_X, SRC_Y + 16, 0.72);                // MIDI
        icon_wave(cr, SRC_X + 18, SRC_Y + 16, 0.55);          // both: audio +
        icon_din(cr, SRC_X + 29, SRC_Y + 16, 0.55);           // MIDI

        set(cr, kAccent);
        sl_brand::draw(cr, 350, 627, 14.0);
    }

    if (layers & kLayerKnobs) {
        const int mainParams[3][4] = {
            { P_BEND, P_STEAL, P_GATE, P_GLIDE },
            { P_VIBDEPTH, P_VIBRATE, P_SHIFT, P_WHEEL },
            { P_TRANSPOSE, P_SCALE, P_ROOT, P_OCTDOWN },
        };
        for (int ry = 0; ry < 3; ++ry) {
            for (int cx = 0; cx < 4; ++cx) {
                const int param = mainParams[ry][cx];
                const char* const label = selectLabel(param, s.value[param]);
                if (kindOf(param) == kSelect)
                    select_badge(cr, MX[cx], MY[ry], 100.0, 25.0, label,
                                 param == P_WHEEL,
                                 static_cast<int>(std::lround(s.value[P_WHEEL])));
                else
                    brass_knob(cr, MX[cx], MY[ry], MAIN_KNOB, norm(s, param));
            }
        }
    }

    if (layers & kLayerSmall) {
        const int small1[5] = { P_BRIGHT_AMT, P_BRIGHT_CC, P_LOUD_AMT, P_LOUD_CC, P_VELNORM };
        for (int i = 0; i < 5; ++i)
            black_knob(cr, SX1[i], SY1, SMALL_KNOB, norm(s, small1[i]));
        const int small2[4] = { P_LOWS, P_SPLIT, P_HIGHCH, P_OUTCHAN };
        for (int i = 0; i < 4; ++i) {
            const char* const label = selectLabel(small2[i], s.value[small2[i]]);
            select_badge(cr, SX2[i], SY2, CH_W, CH_H, label);
        }
        const int track[5] = { P_TRACK_LOW, P_TRACK_RES, P_TRACK_BRIGHT,
                               P_TRACK_JITTER, P_BENDTRIG };
        for (int i = 0; i < 5; ++i)
            black_knob(cr, TR_X[i], TR_Y, SMALL_KNOB, norm(s, track[i]));
    }

    if (layers & kLayerSwitches) {
        slide_switch(cr, SW_X1, SW_Y, s.value[P_MODE] > 0.5f ? 1 : 0);
        slide_switch(cr, SW_X2, SW_Y, s.value[P_LEGATO] > 0.5f ? 1 : 0);
        slide_switch(cr, SRC_X, SRC_Y,
                     static_cast<int>(std::lround(s.value[P_SOURCE])), 3);
    }

    if (layers & kLayerLed)
        led(cr, LED_X, LED_Y, s.value[P_BYPASS] < 0.5f);
}

} // namespace sl_art
