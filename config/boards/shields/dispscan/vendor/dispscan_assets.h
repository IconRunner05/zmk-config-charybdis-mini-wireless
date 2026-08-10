/*
 * dispscan — declarations for the vendored drawing assets.
 *
 * SPDX-License-Identifier: MIT
 *
 * The definitions live in the three vendored translation units next to this
 * header; see vendor/README.md for where each came from and what was changed.
 * Nothing in here is ours except the prefix.
 *
 * All glyphs are LV_COLOR_FORMAT_I1 -- one bit per pixel, with a two-entry
 * palette carried in the first eight bytes of each map. That is the panel's own
 * format, so they blit without a colour conversion and without a canvas: the
 * renderer creates plain lv_image objects and points them at these descriptors.
 * CONFIG_LV_USE_IMAGE=y is what makes that legal; CONFIG_LV_USE_CANVAS is NOT
 * required and is deliberately still off.
 *
 * PALETTE POLARITY, and why the icons come out right on this panel without a
 * recolour. Index 0 is 0xFFFFFFFF (LVGL white) and index 1 is 0x000000FF (LVGL
 * black). custom_status_screen.c documents at length that LVGL's constants are
 * inverted relative to the glass here: LVGL white renders panel-BLACK. So the
 * glyph's index-0 field lands on the same panel-black as COL_BG, and its
 * index-1 ink lands on panel-white -- light content on a dark field, matching
 * the labels drawn beside it. Upstream drew these on an OLED where the same
 * palette also reads light-on-dark, which is why no polarity fix is needed.
 */

#pragma once

#include <lvgl.h>

/* Modifier keycaps, 14x14. Six faces for four modifier CLASSES: the GUI and ALT
 * slots each have a Windows-flavoured and a Mac-flavoured glyph, chosen at
 * build time by CONFIG_DISPSCAN_MAC_MODIFIERS. Left/right are not distinguished
 * because the wire cannot express it (dispscan_status.h, trap #4). */
LV_IMG_DECLARE(dispscan_control_icon);
LV_IMG_DECLARE(dispscan_shift_icon);
LV_IMG_DECLARE(dispscan_alt_icon);
LV_IMG_DECLARE(dispscan_win_icon);
LV_IMG_DECLARE(dispscan_cmd_icon);
LV_IMG_DECLARE(dispscan_opt_icon);

/* Endpoint marks, 9x14. */
LV_IMG_DECLARE(dispscan_sym_usb);
LV_IMG_DECLARE(dispscan_sym_bt);

/* Endpoint states, 5x5. `open` is upstream's name for "advertising, nothing
 * attached" -- the profile is selected but unconnected. */
LV_IMG_DECLARE(dispscan_sym_ok);
LV_IMG_DECLARE(dispscan_sym_nok);
LV_IMG_DECLARE(dispscan_sym_open);

/* 8 px advance, 13 px line height, ASCII 0x20..0x7E. See the banner in
 * dispscan_font_pixel_operator.c for why the advance matters. */
LV_FONT_DECLARE(dispscan_font_pixel_operator);
