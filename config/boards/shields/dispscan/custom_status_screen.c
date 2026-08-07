/*
 * dispscan — placeholder status screen.
 *
 * SPDX-License-Identifier: MIT
 *
 * PLACEHOLDER. This slice is hardware plumbing only: its job is to prove the
 * SPI wiring, the LVGL stack and the ZMK display pipeline all come up, by
 * putting one unmistakable string on the panel. A later slice replaces this
 * whole file with the real widget composition (and keeps "NO SIGNAL" only as
 * the staleness state, per docs/remote-display-plan.md, build order step 4).
 *
 * ZMK calls this from app/src/display/main.c, which declares a weak
 * zmk_display_status_screen() returning NULL; providing a strong definition
 * overrides it. ZMK's own nice_view implementation is kept out of the link by
 * CONFIG_NICE_VIEW_WIDGET_STATUS=n -- see dispscan.conf.
 *
 * LVGL here is 9.3 (ZMK main). Do not port LVGL 8 idioms into this file.
 */

#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <lvgl.h>

#include <zmk/display/status_screen.h>

/*
 * Colour convention is taken from ZMK's nice_view widgets (widgets/util.h):
 * non-inverted means a WHITE background with BLACK content, which is what the
 * Sharp memory LCD shows natively.
 *
 * These are set explicitly rather than left to the theme because the theme is
 * only installed when CONFIG_LV_USE_THEME_MONO is on, and that is implied only
 * by ZMK's built-in status screen -- which this build replaces.
 */

/*
 * Signature intentionally matches include/zmk/display/status_screen.h exactly
 * (empty parameter list, not `void`), so the weak-symbol override binds.
 */
lv_obj_t *zmk_display_status_screen() {
    lv_obj_t *screen = lv_obj_create(NULL);

    lv_obj_set_style_bg_color(screen, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    /* No scrollbars, no elastic scroll: the panel is exactly one screen. */
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *label = lv_label_create(screen);
    /*
     * unscii_8 is a 1bpp bitmap font. At LV_COLOR_DEPTH_1 there is no
     * antialiasing, so an antialiased face (Montserrat) loses its thin strokes
     * entirely. Enabled via the ZMK_LV_FONT_DEFAULT_SMALL choice in
     * Kconfig.defconfig, which `select`s LV_FONT_UNSCII_8.
     */
    lv_obj_set_style_text_font(label, &lv_font_unscii_8, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_black(), LV_PART_MAIN);
    lv_label_set_text(label, "NO SIGNAL");
    /*
     * Centred in the panel's NATIVE orientation (160x68 landscape). On a
     * nice!view mounted on a keyboard the text therefore reads rotated 90
     * degrees; that is expected. Orientation handling is a later slice (see
     * decision D5 -- rotation happens below LVGL in a custom flush callback,
     * never via lv_display_set_rotation()).
     */
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

    LOG_INF("dispscan: placeholder status screen created");

    return screen;
}
