#!/usr/bin/env python3
"""
Checks the generated modgui in a real browser engine (headless Chrome):
renders modgui/icon.html + stylesheet.css, positions every film strip on
the frame mod-ui would show for the given values (same arithmetic as
mod-ui's film widget: rotation = round(norm * steps), shift = -rotation *
element width), screenshots it and compares with modgui/screenshot.png,
the cairo render of the complete pedal at the same values.

usage: modgui_check.py <modgui-dir> <out-dir> <reference.png>
                       sym=value:min:max ... [led=on|off]

<reference.png> is the cairo render of the full pedal at the same values,
at 2x the CSS size (as the modgui images are): it is downsampled here like
the browser downsamples the modgui images, so both go through the same
kind of resampling.
"""

import os
import re
import subprocess
import sys

from PIL import Image, ImageChops, ImageFilter, ImageStat

REGION_MAX = 1.5   # per-control mean |diff| (0..255) after the blur; correct
                   # renders measure <= 0.8, a knob off by one step ~2.7
SELECT_MAX = 14.0  # native <select> elements cannot match the cairo badge
                   # pixel-for-pixel (border antialiasing and font rendering);
                   # a misplaced or wrong-sized select still measures >40
SELECT_ICON_MAX = 19.0  # the chord-wheel cairo badge also draws a chord
                        # symbol, which a native select cannot show


def main():
    mg, out, refpath = sys.argv[1], sys.argv[2], sys.argv[3]
    values, led = {}, "on"
    for a in sys.argv[4:]:
        k, v = a.split("=", 1)
        if k == "led":
            led = v
        else:
            val, lo, hi = (float(x) for x in v.split(":"))
            values[k] = (val - lo) / (hi - lo)

    html = open(os.path.join(mg, "icon.html")).read()
    css = open(os.path.join(mg, "stylesheet.css")).read()
    # strip mustache sections (audio jacks) and namespace placeholders
    html = re.sub(r"\{\{#.*?\}\}.*?\{\{/.*?\}\}", "", html, flags=re.S)
    css = css.replace("{{{ns}}}", "").replace("/resources/", "")
    m = re.search(r"\.sl-pedal \{[^}]*width: ([\d.]+)px; height: ([\d.]+)px", css) or \
        re.search(r"\{[^}]*position: relative; width: ([\d.]+)px; height: ([\d.]+)px", css)
    w, h = int(float(m.group(1))), int(float(m.group(2)))

    script = """
<script>
const values = %s, led = %s;
document.querySelectorAll('[mod-widget=film]').forEach(el => {
  const sym = el.getAttribute('mod-port-symbol');
  if (!(sym in values)) return;
  const cs = getComputedStyle(el);
  const size = parseFloat(cs.width);
  const bs = cs.backgroundSize.split(' ').map(parseFloat);
  const steps = Math.max(1, Math.round(bs[0] / size) - 1);
  const rot = Math.round(values[sym] * steps);
  el.style.backgroundPosition = (-rot * size) + 'px 0px';
});
document.querySelectorAll('[mod-role=bypass-light]').forEach(el => el.classList.add(led));
</script>""" % (repr(values).replace("'", '"'), '"%s"' % led)

    page = os.path.abspath(os.path.join(mg, "_check.html"))
    with open(page, "w") as f:
        f.write("<!DOCTYPE html><html><head><style>body{margin:0;background:#000}%s</style>"
                "</head><body>%s%s</body></html>" % (css, html, script))
    shot = os.path.abspath(os.path.join(out, "modgui-browser.png"))
    try:
        subprocess.run(["google-chrome", "--headless", "--disable-gpu", "--no-sandbox",
                        "--hide-scrollbars", "--force-device-scale-factor=1",
                        "--window-size=%d,%d" % (w, h), "--screenshot=" + shot,
                        "file://" + page], check=True, capture_output=True, timeout=60)
    finally:
        os.remove(page)

    got = Image.open(shot).convert("RGB")
    ref = Image.open(refpath).convert("RGBA").resize((w, h), Image.LANCZOS)
    bg = Image.new("RGBA", ref.size, (0, 0, 0, 255))
    ref = Image.alpha_composite(bg, ref).convert("RGB")
    if got.size != ref.size:
        print("FAIL: browser render %s vs screenshot %s" % (got.size, ref.size))
        return 1
    # The browser downsamples the 2x images while cairo draws at 1x, so
    # edges antialias differently; a slight blur removes most of that.
    # Whole-image statistics cannot see a knob pointer at a wrong angle,
    # so every control box (from the stylesheet) is compared on its own.
    def soften(im):
        return im.filter(ImageFilter.GaussianBlur(1.0))
    diff = ImageChops.difference(soften(got), soften(ref))
    mean = sum(ImageStat.Stat(diff).mean) / 3
    worst = max(b[1] for b in diff.getextrema())
    regions = re.findall(r"\.(\S+) \{ left: (-?[\d.]+)px; top: (-?[\d.]+)px; "
                         r"width: ([\d.]+)px; height: ([\d.]+)px;", css)
    region_fail = []
    for name, l, t, rw, rh in regions:
        l, t, rw, rh = (float(x) for x in (l, t, rw, rh))
        box = (max(0, int(l)), max(0, int(t)), min(w, int(l + rw)), min(h, int(t + rh)))
        rd = sum(ImageStat.Stat(diff.crop(box)).mean) / 3
        limit = SELECT_MAX if name.startswith("sl-select-") else REGION_MAX
        if name == "sl-select-chordwheel":
            limit = SELECT_ICON_MAX
        print("  %-12s mean |diff| %5.2f" % (name, rd))
        if rd > limit:
            region_fail.append(name)
    big = len(region_fail)
    diff.point(lambda p: min(255, p * 4)).save(os.path.join(out, "modgui-diff.png"))
    print("browser vs cairo: whole image mean |diff| %.2f/255, worst %d" % (mean, worst))
    if region_fail:
        print("  mismatching controls: " + ", ".join(region_fail))
    ok = mean < 3.0 and not region_fail
    print("%s: modgui renders like the cairo artwork" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
