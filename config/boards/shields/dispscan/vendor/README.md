# Vendored drawing assets

Third-party art and type used by `custom_status_screen.c`. Everything here is
**data plus a rename** — no upstream logic was imported, and nothing in this
directory knows what a keyboard is.

## Why vendored rather than added as a west module

The obvious move is to put one of these projects in `config/west.yml` and use
its shield. It does not work, and the reason is structural rather than a
packaging detail.

Every ZMK display library — `nice-view-gem`, `zmk-dongle-display`,
`zmk-nice-oled`, `nice-view-elemental` — is a **keyboard-local** status screen.
Its widgets subscribe to ZMK events (`zmk/events/battery_state_changed.h`,
`zmk/events/endpoint_changed.h`, the keymap layer, WPM) and read the state of
*the device they are running on*.

`dispscan` has no keyboard. It is a keyless BLE observer whose entire knowledge
of the world arrives as a 26-byte advertisement and leaves the decoder as a
`struct dispscan_status`. Those listeners would compile, link, and faithfully
render **the scanner's own** empty state: no battery, no layer, no endpoint.
A perfectly drawn screen full of zeroes is a worse failure than an ugly one,
because it looks like it is working.

So the seam holds: the data path stays ours, and only the *presentation* is
borrowed. What is reusable across that seam is exactly what is in this folder —
pixels and glyph metrics.

The one project with our architecture is
[Prospector Scanner](https://github.com/t-ogura/zmk-config-prospector), a
standalone observer reading the same class of status advertisement (its
keyboard-side module is where this repo's broadcaster protocol comes from). It
is bound to a 240x280 colour ST7789 and its README is explicit that nice!view /
Sharp memory LCD is not supported, so none of its rendering transfers to 160x68
at 1bpp. Its LICENSE is also not one GitHub recognises, so nothing was copied
from it — it informed the *feature list* (per-half batteries, RSSI, a swappable
layout) and nothing else.

## Contents

| file | upstream | licence |
| --- | --- | --- |
| `dispscan_sym_mods.c` | [englmaxi/zmk-dongle-display](https://github.com/englmaxi/zmk-dongle-display) `widgets/modifiers_sym.c` | MIT, (c) 2024 The ZMK Contributors |
| `dispscan_sym_output.c` | same repo, `widgets/output_status_sym.c` | MIT, (c) 2024 The ZMK Contributors |
| `dispscan_font_pixel_operator.c` | [M165437/nice-view-gem](https://github.com/M165437/nice-view-gem) `assets/pixel_operator_mono.c` | MIT wrapper, (c) 2024 Michael Schmidt-Voigt; the face is Pixel Operator Mono by Jayvee Enaguas, **CC0 1.0** |

`dispscan_assets.h` is ours and declares all of the above.

Both source projects are MIT, actively maintained as of 2026, and already
written against **LVGL 9** — the same major version ZMK pins here
(`zmkfirmware/lvgl @ f1db87e`, 9.3.0). That last point is why these two were
picked over the other candidates: `nice-view-elemental` and
`zmk-dongle-display-view` are still on LVGL 8 APIs (`lv_canvas_draw_rect`,
`lv_canvas_set_px_color`, `lv_coord_t`) and would have needed a port before a
single pixel appeared.

## Local changes

1. **Every exported symbol gained a `dispscan_` prefix.** `lv_img_dsc_t`
   descriptors and their backing maps are file-scope but not `static`, so
   upstream's `shift_icon`, `control_map` and especially `sym_ok` sit in the
   whole-image namespace next to ZMK, Zephyr and the in-tree `nice_view` shield.
2. **`sym_1` .. `sym_5` were deleted** from `dispscan_sym_output.c`. See the
   banner in that file: upstream maps ZMK's 0-based profile index onto
   one-based digit glyphs, but our `profile_slot` is 0-based and the wire can
   carry 0..7, so the table covers neither end. The digit is drawn as text.
3. Nothing else. No reflow, no reformatting — a diff against upstream should
   show the prefix, the deletion, and the banners, and that is all.

## Re-syncing

These are frozen bitmaps; there is no reason to chase upstream unless a glyph
is redrawn. If you do re-pull, redo the two changes above and keep the diff
mechanical — `sd '\b(control|shift|alt|win|cmd|opt)_icon\b' 'dispscan_${1}_icon'`
and friends is how they were made.

## What was deliberately NOT taken

- **Bongo cat** and the **WPM chart / gauge**: asked for and declined. Both also
  want a signal we do not have — they are driven by a continuous local keypress
  or WPM stream, whereas we learn one WPM byte per beacon, at a cadence that
  drops to ~30 s when the keyboard is idle. A cat that animates once every half
  minute is not the widget anyone was picturing.
- **The battery widget**, which upstream draws on an L8 `lv_canvas`. Taking it
  would mean `CONFIG_LV_USE_CANVAS=y` plus a canvas buffer per battery, to draw
  a rounded rectangle with a fill. The renderer draws its own out of styled
  `lv_obj` rectangles instead — no canvas in the image at all.
