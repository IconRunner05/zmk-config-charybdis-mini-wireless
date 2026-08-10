#!/usr/bin/env python3
"""Render an SVG mock-up of the dispscan status screen.

WHAT THIS IS FOR. Every geometry constant in custom_status_screen.c is derived
on paper -- glyph widths, 8 px font advances, band offsets -- and the only way
to check the derivation used to be to flash a board and photograph it. This
draws the same numbers at the same coordinates so a collision or a clipped field
is visible in a browser instead.

WHAT IT IS NOT. It is not LVGL and it does not prove anything about the
firmware. Icons are exact (their 1bpp bitmaps are parsed straight out of the
vendored .c files), but text is drawn with a generic monospace face at an 8 px
advance rather than with Pixel Operator Mono, so glyph SHAPES are indicative
while POSITIONS and WIDTHS are faithful. If this and the panel ever disagree,
the panel is right.

Coordinates are duplicated from custom_status_screen.c rather than parsed out of
it. That is a deliberate trade: parsing C to draw a picture is its own bug
farm, and a divergence here is caught the moment someone compares the two.
Anything moved there must be moved here.

Usage:
    python3 scripts/dispscan_preview.py [-o out.svg] [--scale N]
"""

import argparse
import pathlib
import re
import sys

# --- panel ------------------------------------------------------------------
PANEL_W, PANEL_H = 160, 68

# Panel-black field, panel-white ink. See the colour block in
# custom_status_screen.c: LVGL's constants are inverted relative to this glass,
# and these are the colours as they actually appear.
FIELD = "#101010"
INK = "#f4f4f4"

# --- geometry, mirrored from custom_status_screen.c -------------------------
MARGIN = 2
BAND_A_Y, BAND_B_Y, BAND_C_Y, BAND_D_Y = 0, 18, 35, 54

EP_USB_X, EP_USB_STATE_X = 2, 12
EP_BT_X, EP_BT_DIGIT_X, EP_BT_STATE_X = 22, 32, 41
EP_STATE_DY = 9

BATT_L_SIDE_X, BATT_L_GAUGE_X, BATT_L_TEXT_X = 2, 12, 38
BATT_R_SIDE_X, BATT_R_GAUGE_X, BATT_R_TEXT_X = 84, 94, 120
GAUGE_W, GAUGE_H = 22, 11
GAUGE_FILL_MAX_W, GAUGE_FILL_H, GAUGE_FILL_INSET = 18, 7, 2
GAUGE_NUB_W, GAUGE_NUB_H, GAUGE_DY = 2, 5, 1

MOD_COUNT, MOD_ICON_W, MOD_PITCH, MOD_FIRST_X = 4, 14, 18, 2
MOD_UNDERLINE_DY, MOD_UNDERLINE_H = 15, 1
MARKER_DY, STALE_MARKER_X = 4, 82

CHAR_W = 8  # both faces
FONT_TALL, FONT_SMALL = 13, 9

VENDOR = pathlib.Path(__file__).resolve().parent.parent / (
    "config/boards/shields/dispscan/vendor"
)


def parse_glyphs(path):
    """Pull every `lv_img_dsc_t` out of a vendored C file as a pixel matrix.

    The maps are I1: eight bytes of palette, then one row at a time, each row
    padded up to a byte boundary, most significant bit leftmost. Index 1 is the
    ink.
    """
    src = path.read_text()
    maps = {
        m.group(1): m.group(2)
        for m in re.finditer(r"uint8_t (\w+_map)\[\] = \{(.*?)\};", src, re.S)
    }

    def field(pattern, body, name):
        """Required field. A vendored file that does not match has been edited
        into a shape this parser does not understand -- say so, rather than
        raising an AttributeError on None ten lines later."""
        m = re.search(pattern, body)
        if m is None:
            sys.exit(f"{path.name}: {name}: no match for {pattern!r}")
        return m.group(1)

    out = {}
    for m in re.finditer(r"const lv_img_dsc_t (\w+) = \{(.*?)\};", src, re.S):
        name, body = m.group(1), m.group(2)
        w = int(field(r"header\.w = (\d+)", body, name))
        h = int(field(r"header\.h = (\d+)", body, name))
        data = field(r"\.data = (\w+)", body, name)
        raw = [int(b, 16) for b in re.findall(r"0x([0-9a-fA-F]{2})", maps[data])]
        raw = raw[8:]  # drop the two 4-byte palette entries
        stride = (w + 7) // 8
        rows = []
        for y in range(h):
            row = []
            for x in range(w):
                byte = raw[y * stride + (x >> 3)]
                row.append((byte >> (7 - (x & 7))) & 1)
            rows.append(row)
        out[name] = rows
    return out


def rect(x, y, w, h, fill=INK):
    return f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="{fill}"/>'


def glyph(px, x, y):
    """Emit one rect per ink pixel. Verbose, but exact and trivially checkable."""
    parts = []
    for dy, row in enumerate(px):
        dx = 0
        while dx < len(row):
            if row[dx]:
                run = dx
                while run < len(row) and row[run]:
                    run += 1
                parts.append(rect(x + dx, y + dy, run - dx, 1))
                dx = run
            else:
                dx += 1
    return "".join(parts)


def text(s, x, y, size, anchor="start"):
    """Text on the 8 px grid. `y` is the band top; shift to a baseline."""
    baseline = y + size - 2
    return (
        f'<text x="{x}" y="{baseline}" fill="{INK}" text-anchor="{anchor}" '
        f'font-family="DejaVu Sans Mono, Menlo, monospace" font-size="{size}" '
        f'textLength="{len(s) * CHAR_W}" lengthAdjust="spacingAndGlyphs" '
        f'xml:space="preserve">{s}</text>'
    )


def gauge(x, pct):
    """A battery. Outline for any legal reading incl. N/A; none above 100."""
    if pct > 100:
        return ""
    y = BAND_B_Y + GAUGE_DY
    parts = [
        # 1 px border, drawn as four edges so the field shows through
        rect(x, y, GAUGE_W, 1),
        rect(x, y + GAUGE_H - 1, GAUGE_W, 1),
        rect(x, y, 1, GAUGE_H),
        rect(x + GAUGE_W - 1, y, 1, GAUGE_H),
        rect(x + GAUGE_W, y + (GAUGE_H - GAUGE_NUB_H) // 2, GAUGE_NUB_W, GAUGE_NUB_H),
    ]
    if pct > 0:
        w = max(1, (pct * GAUGE_FILL_MAX_W + 50) // 100)
        parts.append(rect(x + GAUGE_FILL_INSET, y + GAUGE_FILL_INSET, w, GAUGE_FILL_H))
    return "".join(parts)


def batt_text(pct):
    return "  NA" if pct == 0 else f"{pct:3d}%"


def screen(g, state):
    """One 160x68 frame."""
    p = [rect(0, 0, PANEL_W, PANEL_H, FIELD)]

    if state.get("nosig"):
        p.append(text("-- NO SIGNAL --", PANEL_W // 2, 21, FONT_TALL, "middle"))
        p.append(text("no beacon heard", PANEL_W // 2, 37, FONT_SMALL, "middle"))
        return "".join(p)
    if state.get("dark"):
        return "".join(p)

    # band A -- endpoint cluster, then the layer
    p.append(glyph(g["dispscan_sym_usb"], EP_USB_X, BAND_A_Y))
    p.append(glyph(g[state["usb"]], EP_USB_STATE_X, BAND_A_Y + EP_STATE_DY))
    p.append(glyph(g["dispscan_sym_bt"], EP_BT_X, BAND_A_Y))
    p.append(text(state["profile"], EP_BT_DIGIT_X, BAND_A_Y, FONT_TALL))
    p.append(glyph(g[state["bt"]], EP_BT_STATE_X, BAND_A_Y + EP_STATE_DY))
    p.append(text(state["layer"], PANEL_W - MARGIN, BAND_A_Y, FONT_TALL, "end"))

    # band B -- batteries
    for side, sx, gx, tx, pct in (
        ("L", BATT_L_SIDE_X, BATT_L_GAUGE_X, BATT_L_TEXT_X, state["battl"]),
        ("R", BATT_R_SIDE_X, BATT_R_GAUGE_X, BATT_R_TEXT_X, state["battr"]),
    ):
        p.append(text(side, sx, BAND_B_Y, FONT_TALL))
        p.append(gauge(gx, pct))
        p.append(text(batt_text(pct), tx, BAND_B_Y, FONT_TALL))

    # band C -- modifier keycaps with an underline under the held ones
    icons = [
        "dispscan_control_icon",
        "dispscan_shift_icon",
        "dispscan_alt_icon",
        "dispscan_win_icon",
    ]
    for i, name in enumerate(icons):
        x = MOD_FIRST_X + i * MOD_PITCH
        p.append(glyph(g[name], x, BAND_C_Y))
        if state["mods"][i]:
            p.append(rect(x, BAND_C_Y + MOD_UNDERLINE_DY, MOD_ICON_W, MOD_UNDERLINE_H))
    if state.get("stale"):
        p.append(text("STALE", STALE_MARKER_X, BAND_C_Y + MARKER_DY, FONT_SMALL))
    if state.get("caps"):
        p.append(
            text("CAPS", PANEL_W - MARGIN, BAND_C_Y + MARKER_DY, FONT_SMALL, "end")
        )

    # band D
    p.append(text(f"WPM {state['wpm']}", MARGIN, BAND_D_Y, FONT_SMALL))
    p.append(text(state["right"], PANEL_W - MARGIN, BAND_D_Y, FONT_SMALL, "end"))
    return "".join(p)


# The frames worth looking at: the ordinary one, the two extremes the fake
# source emits on purpose, and the two non-AWAKE states.
FRAMES = [
    (
        "AWAKE — typical, discovery mode",
        dict(
            usb="dispscan_sym_open",
            bt="dispscan_sym_ok",
            profile="1",
            layer="L2 NAV",
            battl=87,
            battr=92,
            mods=[0, 1, 0, 1],
            wpm=42,
            caps=False,
            stale=False,
            right="ID C0FFEE12",
        ),
    ),
    (
        "AWAKE — bound build, RSSI in band D, left half N/A",
        dict(
            usb="dispscan_sym_ok",
            bt="dispscan_sym_open",
            profile="0",
            layer="L0 BASE",
            battl=0,
            battr=64,
            mods=[0, 0, 0, 0],
            wpm=0,
            caps=False,
            stale=True,
            right="-58dBm",
        ),
    ),
    (
        "AWAKE — worst case: widest layer, out-of-range batteries, bad profile",
        dict(
            usb="dispscan_sym_open",
            bt="dispscan_sym_open",
            profile="?",
            layer="L255 ABCD",
            battl=255,
            battr=101,
            mods=[1, 1, 1, 1],
            wpm=255,
            caps=True,
            stale=True,
            right="ID C0FFEE12",
        ),
    ),
    ("DARK — keyboard idle, data still good", dict(dark=True)),
    ("NO SIGNAL — nothing heard", dict(nosig=True)),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--out", default="dispscan_preview.svg")
    ap.add_argument("--scale", type=int, default=4)
    args = ap.parse_args()

    g = {}
    for f in ("dispscan_sym_mods.c", "dispscan_sym_output.c"):
        path = VENDOR / f
        if not path.exists():
            sys.exit(f"missing vendored glyphs: {path}")
        g.update(parse_glyphs(path))

    s = args.scale
    pad, gap, label_h = 12, 14, 16
    row_h = PANEL_H * s + label_h + gap
    width = PANEL_W * s + 2 * pad
    height = pad + len(FRAMES) * row_h

    out = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" '
        f'height="{height}" viewBox="0 0 {width} {height}">',
        f'<rect width="{width}" height="{height}" fill="#242424"/>',
    ]
    for i, (caption, st) in enumerate(FRAMES):
        y = pad + i * row_h
        out.append(
            f'<text x="{pad}" y="{y + 11}" fill="#b9b9b9" font-size="11" '
            f'font-family="system-ui, sans-serif">{caption}</text>'
        )
        out.append(
            f'<g transform="translate({pad},{y + label_h}) scale({s})" '
            f'shape-rendering="crispEdges">{screen(g, st)}</g>'
        )
    out.append("</svg>")

    pathlib.Path(args.out).write_text("\n".join(out))
    print(f"wrote {args.out}  ({len(FRAMES)} frames, {PANEL_W}x{PANEL_H} at {s}x)")


if __name__ == "__main__":
    main()
