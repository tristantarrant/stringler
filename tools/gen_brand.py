#!/usr/bin/env python3
"""
Bake a wordmark into a cairo path table for plugins/Stringler/Artwork.hpp.

The GUI must look the same on machines without the fonts (and in the
modgui, which is rendered at build time), so the glyph outlines are
emitted as a compact path table instead of relying on the system font
at run time.

  fonts/BitcountGridDouble.ttf   the variable wordmark font (SIL OFL 1.1,
                                 OFL-Bitcount.txt)

Usage:
  tools/gen_brand.py --font Bitcount --text dataforte --ns em_brand --out BrandPath.hpp
"""

import argparse
import math
import os
import sys

from fontTools.pens.basePen import BasePen
from fontTools.pens.recordingPen import DecomposingRecordingPen
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
FONTS = os.path.join(ROOT, "plugins", "Stringler", "fonts")

FONT_FILES = {
    "Bitcount": os.path.join(FONTS, "BitcountGridDouble.ttf"),
}


class CairoPen(BasePen):
    """Collect a glyph outline as cairo path ops in font units, flattening
    curves into lines: at the sizes the wordmark is drawn, curves would
    cost five times the data for no visible difference."""

    def __init__(self, glyphSet, tol=2.0):
        super().__init__(glyphSet)
        self.ops = []
        self.tol = tol

    def _moveTo(self, p):
        self.ops.append(("M",) + p)

    def _lineTo(self, p):
        self._add_line(p)

    def _curveToOne(self, p1, p2, p3):
        p0 = self._getCurrentPoint()
        d = (abs(p0[0] - 2 * p1[0] + p2[0]) + abs(p0[1] - 2 * p1[1] + p2[1])
             + abs(p1[0] - 2 * p2[0] + p3[0]) + abs(p1[1] - 2 * p2[1] + p3[1]))
        n = max(2, min(10, int(round((d / (4.0 * self.tol)) ** 0.5)) + 1))
        for i in range(1, n + 1):
            t = i / float(n)
            mt = 1.0 - t
            x = (mt ** 3 * p0[0] + 3 * mt ** 2 * t * p1[0]
                 + 3 * mt * t ** 2 * p2[0] + t ** 3 * p3[0])
            y = (mt ** 3 * p0[1] + 3 * mt ** 2 * t * p1[1]
                 + 3 * mt * t ** 2 * p2[1] + t ** 3 * p3[1])
            self._add_line((x, y))

    def _qCurveToOne(self, p1, p2):
        p0 = self._getCurrentPoint()
        d = math.hypot(p0[0] - 2 * p1[0] + p2[0], p0[1] - 2 * p1[1] + p2[1])
        n = max(2, min(16, int(math.ceil((d / (8.0 * self.tol)) ** 0.5))))
        for i in range(1, n + 1):
            t = i / float(n)
            mt = 1.0 - t
            x = mt * mt * p0[0] + 2 * mt * t * p1[0] + t * t * p2[0]
            y = mt * mt * p0[1] + 2 * mt * t * p1[1] + t * t * p2[1]
            self._add_line((x, y))

    def _closePath(self):
        self._simplify_last()
        self.ops.append(("Z",))

    def _endPath(self):
        self._closePath()

    def _add_line(self, p):
        if self.ops and self.ops[-1][0] in ("L", "M"):
            if abs(self.ops[-1][1] - p[0]) < 0.4 and abs(self.ops[-1][2] - p[1]) < 0.4:
                return
        self.ops.append(("L",) + p)

    def _simplify_last(self):
        start = 0
        for i, op in enumerate(self.ops):
            if op[0] == "M":
                start = i
        pts = [op for op in self.ops[start:] if op[0] in ("M", "L")]
        if len(pts) < 3:
            return
        keep = [pts[0]]
        for i in range(1, len(pts) - 1):
            a, b, c = keep[-1], pts[i], pts[i + 1]
            cross = (b[1] - a[1]) * (c[2] - a[2]) - (b[2] - a[2]) * (c[1] - a[1])
            dot = (b[1] - a[1]) * (c[1] - b[1]) + (b[2] - a[2]) * (c[2] - b[2])
            if abs(cross) > 0.5 or dot < 0:
                keep.append(b)
        keep.append(pts[-1])
        tail = [op for op in self.ops[start:] if op[0] not in ("M", "L")]
        self.ops[start:] = keep + tail


def layout(font, text):
    glyphSet = font.getGlyphSet()
    cmap = font.getBestCmap()
    hmtx = font["hmtx"]
    kern = {}
    if "kern" in font:
        try:
            for st in font["kern"].kernTables:
                kern.update(st.kernTable)
        except Exception:
            pass
    x = 0.0
    ops = []
    prev = None
    for ch in text:
        name = cmap.get(ord(ch))
        if name is None:
            if ch == " ":
                x += font["head"].unitsPerEm * 0.25
                prev = None
                continue
            sys.exit("font has no glyph for %r" % ch)
        if prev is not None:
            x += kern.get((prev, name), 0)
        rec = DecomposingRecordingPen(glyphSet)
        glyphSet[name].draw(rec)
        pen = CairoPen(glyphSet)
        rec.replay(pen)
        for op in pen.ops:
            if op[0] == "Z":
                ops.append(op)
            else:
                vals = list(op[1:])
                for i in range(0, len(vals), 2):
                    vals[i] += x
                ops.append((op[0],) + tuple(vals))
        x += hmtx[name][0]
        prev = name
    return ops, x


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--font", choices=sorted(FONT_FILES), required=True)
    ap.add_argument("--text", required=True)
    ap.add_argument("--ns", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--weight", type=float, default=700)
    args = ap.parse_args()

    font = TTFont(FONT_FILES[args.font])
    if "fvar" in font:
        font = instantiateVariableFont(font, {"wght": args.weight})
    upem = font["head"].unitsPerEm
    ops, width = layout(font, args.text)

    table = []
    for op in ops:
        if op[0] == "Z":
            table.append(-1)
        else:
            code = {"M": 0, "L": 1, "C": 2}[op[0]]
            table.append(code)
            for v in op[1:]:
                table.append(int(round(v)))
    assert all(-32768 <= v <= 32767 for v in table), "coordinates exceed int16"

    src = os.path.relpath(FONT_FILES[args.font], os.path.join(ROOT, "plugins", "Stringler"))
    lines = []
    lines.append("/*")
    lines.append(' * Stringler - the "%s" wordmark, baked from %s' % (args.text, src))
    lines.append(" * (SIL Open Font License 1.1, fonts/).")
    lines.append(" *")
    lines.append(" * GENERATED by tools/gen_brand.py -- do not edit by hand.")
    lines.append(" *")
    lines.append(" * Glyph outlines as a compact cairo path table in font units")
    lines.append(" * (%d units per em, y up). Baking the outline keeps the plugin UI" % upem)
    lines.append(" * and the modgui identical on machines without the font.")
    lines.append(" *")
    lines.append(" * Copyright (C) 2026 Dataforte")
    lines.append(" * SPDX-License-Identifier: GPL-2.0-or-later")
    lines.append(" */")
    lines.append("")
    lines.append("#pragma once")
    lines.append("")
    lines.append("#include <cairo.h>")
    lines.append("")
    lines.append("namespace %s {" % args.ns)
    lines.append("")
    lines.append("constexpr double kUnitsPerEm = %d.0;" % upem)
    lines.append("constexpr double kWidth      = %.1f;   // advance width in font units" % width)
    lines.append("")
    lines.append("// 0 = move, 1 = line, -1 = close")
    lines.append("constexpr short kPath[] = {")
    row = []
    for v in table:
        row.append("%d," % v)
        if len(row) == 16:
            lines.append("    " + " ".join(row))
            row = []
    if row:
        lines.append("    " + " ".join(row))
    lines.append("};")
    lines.append("")
    lines.append("""// Draw the wordmark centred on x with its baseline at y, `size` units
// tall (em size, like cairo_set_font_size).
inline void draw(cairo_t* cr, double x, double baseline, double size)
{
    const double s = size / kUnitsPerEm;
    int i = 0;
    cairo_save(cr);
    cairo_translate(cr, x - 0.5 * kWidth * s, baseline);
    cairo_scale(cr, s, -s);          // font units are y up
    while (i < static_cast<int>(sizeof(kPath) / sizeof(kPath[0]))) {
        const int op = kPath[i++];
        if (op < 0) {
            cairo_close_path(cr);
        } else if (op == 0) {
            cairo_move_to(cr, kPath[i], kPath[i + 1]);
            i += 2;
        } else {
            cairo_line_to(cr, kPath[i], kPath[i + 1]);
            i += 2;
        }
    }
    cairo_fill(cr);
    cairo_restore(cr);
    cairo_new_path(cr);
}
""")
    lines.append("} // namespace %s" % args.ns)
    lines.append("")

    out = os.path.join(ROOT, "plugins", "Stringler", args.out)
    with open(out, "w") as f:
        f.write("\n".join(lines))
    print("wrote %s (%d path values, %.0f units wide, upem %d)"
          % (os.path.relpath(out, ROOT), len(table), width, upem))


if __name__ == "__main__":
    main()
