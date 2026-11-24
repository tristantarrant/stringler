/*
 * Renders the modgui images for Stringler from the same cairo artwork as
 * the plugin UI (plugins/Stringler/Artwork.hpp).
 *
 *   render_modgui <outdir>            modgui images, icon.html, stylesheet.css
 *                                     and modgui.ttl (positions come from the
 *                                     artwork's control layout)
 *   render_modgui --preview <file> [scale [v0 v1 ...]]
 *                                     full pedal; optional parameter values
 *                                     in parameter order (default: defaults)
 *
 * Copyright (C) 2026 Dataforte
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "../plugins/Stringler/Artwork.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "../plugins/Stringler/ParamNames.hpp"

using namespace sl_art;

// modgui CSS size of the pedal = design size * CSS_SCALE; images are
// rendered at IMG_SCALE times that for high-DPI screens.
static constexpr double CSS_SCALE = 0.8;
static constexpr double IMG_SCALE = 2.0;
static constexpr int KNOB_FRAMES = 61;

// Film-strip boxes in design units, chosen so frames are whole pixels at
// both CSS scale (x0.8) and image scale (x1.6): mod-ui derives the frame
// count from the strip width. Every box edge (centre minus half the box)
// must land on a whole CSS pixel, i.e. be a multiple of 1.25 design units;
// px() below refuses fractional positions.
static constexpr double MAIN_BOX = 62.5;
static constexpr double SMALL_BOX = 42.5;
static constexpr double SWITCH_BOX = 35.0;
static constexpr double LED_BOX = 30.0;
static constexpr double FOOT_BOX = 50.0;

static bool g_fractional = false;

static std::string px(double design)
{
    const double v = design * CSS_SCALE;
    if (std::fabs(v - std::round(v)) > 1e-6) {
        std::fprintf(stderr, "error: CSS value %g px is not a whole pixel\n", v);
        g_fractional = true;
    }
    char b[32];
    std::snprintf(b, sizeof b, "%gpx", design * CSS_SCALE);
    return b;
}

// absolutely positioned box of `box` design units centred on (x, y)
static std::string place(double x, double y, double box)
{
    return "left: " + px(x - box / 2) + "; top: " + px(y - box / 2) +
           "; width: " + px(box) + "; height: " + px(box) + ";";
}

// absolutely positioned box with explicit width and height
static std::string placeWH(double x, double y, double w, double h)
{
    return "left: " + px(x - w / 2) + "; top: " + px(y - h / 2) +
           "; width: " + px(w) + "; height: " + px(h) + ";";
}

static std::vector<std::pair<int, const char*>> optionsFor(int param)
{
    int count = 0;
    const sl_names::Item* const items = selectItems(param, count);
    std::vector<std::pair<int, const char*>> out;
    for (int i = 0; items != nullptr && i < count; ++i)
        out.emplace_back(items[i].value, items[i].label);
    return out;
}

static std::string film(const char* img, int frames, double box)
{
    return std::string("background-image: url(/resources/") + img +
           "{{{ns}}}); background-repeat: no-repeat; background-size: " +
           px(box * frames) + " " + px(box) + ";";
}

static bool write_text(const std::string& path, const std::string& body)
{
    std::ofstream f(path);
    f << body;
    std::printf("wrote %s\n", path.c_str());
    return bool(f);
}

static const char* kJacks = R"(    <div class="mod-pedal-input">
        {{#effect.ports.audio.input}}
        <div class="mod-input mod-input-disconnected" title="{{name}}" mod-role="input-audio-port" mod-port-symbol="{{symbol}}">
            <div class="mod-pedal-input-image"></div>
        </div>
        {{/effect.ports.audio.input}}
    </div>
    <div class="mod-pedal-output">
        {{#effect.ports.audio.output}}
        <div class="mod-output mod-output-disconnected" title="{{name}}" mod-role="output-audio-port" mod-port-symbol="{{symbol}}">
            <div class="mod-pedal-output-image"></div>
        </div>
        {{/effect.ports.audio.output}}
    </div>
)";

// the LV2 parameter symbol for an artwork control
static const char* symOf(const Control& c)
{
    switch (c.param) {
    case P_MODE:       return "mode";
    case P_BEND:       return "bend";
    case P_STEAL:      return "steal";
    case P_GATE:       return "gate";
    case P_LEGATO:     return "legato";
    case P_GLIDE:      return "legatoms";
    case P_VIBDEPTH:   return "vibdepth";
    case P_VIBRATE:    return "vibrate";
    case P_SHIFT:      return "chordshift";
    case P_WHEEL:      return "chordwheel";
    case P_TRANSPOSE:  return "transpose";
    case P_SCALE:      return "scale";
    case P_ROOT:       return "root";
    case P_OCTDOWN:    return "octdown";
    case P_SPLIT:      return "split";
    case P_LOWS:       return "lowschan";
    case P_HIGHCH:     return "highschan";
    case P_BRIGHT_AMT: return "brightamt";
    case P_BRIGHT_CC:  return "brightcc";
    case P_LOUD_AMT:   return "loudamt";
    case P_LOUD_CC:    return "loudcc";
    case P_VELNORM:    return "velnorm";
    case P_OUTCHAN:    return "outchan";
    case P_SOURCE:     return "source";
    case P_TRACK_LOW:  return "tracklow";
    case P_TRACK_RES:  return "trackres";
    case P_TRACK_BRIGHT: return "trackbright";
    case P_TRACK_JITTER: return "trackjitter";
    case P_BENDTRIG:   return "bendtrig";
    case P_BYPASS:     return "bypass";
    default:            return "?";
    }
}

static double boxOf(const Control& c)
{
    switch (c.kind) {
    case kKnob:
        return (c.r >= MAIN_RING) ? MAIN_BOX : SMALL_BOX;
    case kToggle:
        return SWITCH_BOX;
    case kSelect:
        return c.w;
    case kFootswitch:
        return FOOT_BOX;
    default:
        return 0.0;
    }
}

static bool write_modgui_text(const std::string& dir)
{
    std::string html =
        "<div class=\"mod-pedal sl-pedal\">\n"
        "    <div mod-role=\"drag-handle\" class=\"mod-drag-handle\"></div>\n";
    std::string css = "/* generated by tools/render_modgui.cpp from Artwork.hpp -- do not edit */\n";
    css += ".sl-pedal { position: relative; width: " + px(W) + "; height: " + px(H) +
           "; background-image: url(/resources/pedal.png{{{ns}}}); background-size: " +
           px(W) + " " + px(H) + "; background-repeat: no-repeat; }\n";
    css += ".sl-pedal .sl-select { position: absolute; }\n";
    css += ".sl-pedal .sl-select select { width: 100%; height: 100%; box-sizing: border-box; "
           "background: #12100d; color: #d9ccb8; border: 1px solid rgba(199,120,64,0.55); "
           "border-radius: 4px; font-family: \"DejaVu Sans Condensed\", sans-serif; "
           "font-weight: bold; font-size: 8px; padding: 0 12px 0 6px; line-height: 18px; "
           "appearance: none; -webkit-appearance: none; cursor: pointer; }\n";
    css += ".sl-pedal .sl-caret { position: absolute; right: 8px; top: 50%; margin-top: -2px; "
           "width: 0; height: 0; border-left: 3px solid transparent; "
           "border-right: 3px solid transparent; border-top: 4px solid #cfc8b8; "
           "opacity: 0.75; pointer-events: none; }\n";
    css += ".sl-pedal .sl-knob, .sl-pedal .sl-toggle, .sl-pedal .sl-led,"
           " .sl-pedal .sl-foot { position: absolute; cursor: pointer; }\n";

    for (const Control& c : kControls) {
        const char* sym = symOf(c);
        const double box = boxOf(c);

        if (c.kind == kFootswitch) {
            html += "    <div class=\"sl-foot mod-footswitch\" mod-role=\"bypass\"></div>\n";
            css += ".sl-pedal .sl-foot { " + place(c.x, c.y, box) +
                   " border-radius: 50%; }\n";
            continue;
        }

        if (c.kind == kSelect) {
            html += std::string("    <div class=\"sl-select sl-select-") + sym + "\">\n";
            html += std::string("        <select mod-role=\"input-control-port\" mod-port-symbol=\"") +
                    sym + "\" mod-widget=\"select\" title=\"" + sym + "\">\n";
            for (const auto& opt : optionsFor(c.param))
                html += std::string("            <option value=\"") +
                        std::to_string(opt.first) + "\">" + opt.second + "</option>\n";
            html += "        </select>\n";
            html += "        <span class=\"sl-caret\">\xe2\x96\xbe</span>\n";
            html += "    </div>\n";
            css += std::string(".sl-pedal .sl-select-") + sym + " { " +
                   placeWH(c.x, c.y, c.w, c.h) + " }\n";
            continue;
        }

        html += std::string("    <div class=\"sl-") + (c.kind == kToggle ? "toggle" : "knob") +
                " sl-" + sym + "\" mod-role=\"input-control-port\" mod-port-symbol=\"" +
                sym + "\" mod-widget=\"film\"></div>\n";

        if (c.kind == kKnob)
            css += std::string(".sl-pedal .sl-") + sym + " { " +
                   place(c.x, c.y, box) + " " +
                   film((c.r >= MAIN_RING) ? "mainknob.png" : "smallknob.png",
                        KNOB_FRAMES, box) + " }\n";
        else
            css += std::string(".sl-pedal .sl-") + sym + " { " + place(c.x, c.y, box) + " " +
                   film(c.max >= 2.0 ? "switch3.png" : "switch.png",
                        static_cast<int>(c.max) + 1, box) + " }\n";
    }

    html += "    <div class=\"sl-led mod-light\" mod-role=\"bypass-light\"></div>\n";
    css += ".sl-pedal .sl-led { " + place(LED_X, LED_Y, LED_BOX) + " " +
           film("led.png", 2, LED_BOX) + " }\n";
    css += ".sl-pedal .sl-led.on { background-position: -" + px(LED_BOX) + " 0px; }\n";

    html += kJacks;
    html += "</div>\n";

    const std::string ttl =
        "@prefix modgui: <http://moddevices.com/ns/modgui#> .\n"
        "@prefix lv2:    <http://lv2plug.in/ns/lv2core#> .\n\n"
        "<http://dataforte.net/plugins/stringler>\n"
        "    modgui:gui [\n"
        "        modgui:resourcesDirectory <modgui> ;\n"
        "        modgui:iconTemplate <modgui/icon.html> ;\n"
        "        modgui:stylesheet <modgui/stylesheet.css> ;\n"
        "        modgui:screenshot <modgui/screenshot.png> ;\n"
        "        modgui:thumbnail <modgui/thumbnail.png> ;\n"
        "        modgui:brand \"Dataforte\" ;\n"
        "        modgui:label \"Stringler\" ;\n"
        "    ] .\n";

    return write_text(dir + "/icon.html", html) &&
           write_text(dir + "/stylesheet.css", css) &&
           write_text(dir + "/modgui.ttl", ttl);
}

static bool save(cairo_surface_t* s, const std::string& path)
{
    if (cairo_surface_write_to_png(s, path.c_str()) != CAIRO_STATUS_SUCCESS) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return false;
    }
    std::printf("wrote %s (%dx%d)\n", path.c_str(),
                cairo_image_surface_get_width(s), cairo_image_surface_get_height(s));
    return true;
}

static cairo_surface_t* surface(double w, double h, double scale, cairo_t** cr)
{
    cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                    (int)std::lround(w * scale),
                                                    (int)std::lround(h * scale));
    *cr = cairo_create(s);
    cairo_scale(*cr, scale, scale);
    return s;
}

static bool pedal(const std::string& path, unsigned layers, double scale, const State& st = State())
{
    cairo_t* cr;
    cairo_surface_t* s = surface(W, H, scale, &cr);
    draw(cr, st, layers);
    cairo_destroy(cr);
    const bool ok = save(s, path);
    cairo_surface_destroy(s);
    return ok;
}

// Horizontal film strip: frame i draws `fn(cr, i)` in a box of `box` units.
template <typename F>
static bool strip(const std::string& path, int frames, double box, double scale, F fn)
{
    cairo_t* cr;
    cairo_surface_t* s = surface(box * frames, box, scale, &cr);
    for (int i = 0; i < frames; ++i) {
        cairo_save(cr);
        cairo_translate(cr, i * box + box / 2, box / 2);
        fn(cr, i);
        cairo_restore(cr);
    }
    cairo_destroy(cr);
    const bool ok = save(s, path);
    cairo_surface_destroy(s);
    return ok;
}

int main(int argc, char** argv)
{
    if (argc >= 3 && std::string(argv[1]) == "--preview") {
        const double scale = argc >= 4 ? std::atof(argv[3]) : 1.0;
        State st;
        for (int i = 4; i < argc && i - 4 < P_COUNT; ++i)
            st.value[i - 4] = static_cast<float>(std::atof(argv[i]));
        return pedal(argv[2], kLayerAll, scale, st) ? 0 : 1;
    }
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <outdir> | --preview <file.png> [scale]\n", argv[0]);
        return 1;
    }
    const std::string dir = argv[1];
    const double sc = CSS_SCALE * IMG_SCALE;   // design units -> image pixels
    bool ok = true;

    // static background: everything but knobs, switches and LED
    ok &= pedal(dir + "/pedal.png", kLayerBody, sc);

    // main brass knobs film strip
    ok &= strip(dir + "/mainknob.png", KNOB_FRAMES, MAIN_BOX, sc, [](cairo_t* cr, int i) {
        brass_knob(cr, 0, 0, MAIN_KNOB, i / double(KNOB_FRAMES - 1));
    });

    // small black knobs film strip
    ok &= strip(dir + "/smallknob.png", KNOB_FRAMES, SMALL_BOX, sc, [](cairo_t* cr, int i) {
        black_knob(cr, 0, 0, SMALL_KNOB, i / double(KNOB_FRAMES - 1));
    });

    ok &= strip(dir + "/switch.png", 2, SWITCH_BOX, sc, [](cairo_t* cr, int i) {
        slide_switch(cr, 0, 0, i, 2);
    });

    ok &= strip(dir + "/switch3.png", 3, SWITCH_BOX, sc, [](cairo_t* cr, int i) {
        slide_switch(cr, 0, 0, i, 3);
    });

    ok &= strip(dir + "/led.png", 2, LED_BOX, sc, [](cairo_t* cr, int i) {
        led(cr, 0, 0, i == 1);
    });

    // screenshot (CSS size) and thumbnail
    ok &= pedal(dir + "/screenshot.png", kLayerAll, CSS_SCALE);
    ok &= pedal(dir + "/thumbnail.png", kLayerAll, CSS_SCALE * 0.3);

    ok &= write_modgui_text(dir);
    if (g_fractional) {
        std::fprintf(stderr, "error: fix the layout in Artwork.hpp (positions must snap to "
                             "1.25 design units)\n");
        return 1;
    }
    return ok ? 0 : 1;
}
