/*
 * dispscan — landscape status screen for the nice!view (160x68, 1bpp).
 *
 * SPDX-License-Identifier: MIT
 *
 * Driven entirely through dispscan_status_update(); the callers are
 * dispscan_fake_source.c (dev) and dispscan_observer.c (real). There is no BLE
 * code here and none is wanted -- see dispscan_status.h for the seam.
 *
 * ZMK calls zmk_display_status_screen() from app/src/display/main.c, which
 * declares it __attribute__((weak)) returning NULL (main.c:37); providing a
 * strong definition overrides it. ZMK's own nice_view implementation is kept out
 * of the link by CONFIG_NICE_VIEW_WIDGET_STATUS=n -- see dispscan.conf.
 *
 * LVGL here is 9.3.0 (zmkfirmware/lvgl @ f1db87e, pinned in zmk/app/west.yml:38).
 * Do not port LVGL 8 idioms into this file.
 *
 * NO ROTATION ANYWHERE. D5 rules landscape-only and the panel's native
 * orientation IS 160x68 landscape, so composition is plain lv_obj_align().
 * lv_display_set_rotation() must never be called: at LV_COLOR_FORMAT_I1 the
 * software transform path has no case for I1 and would fail silently, producing
 * a correctly-laid-out scene in a mis-shaped buffer (plan doc, D5 finding 2).
 *
 * -------------------------------------------------------------------------
 * PRESENTATION: ICONS AND A PROPER PIXEL FACE, NOT ASCII
 *
 * This screen used to be text only -- unscii_8 everywhere, batteries as
 * "L  87% [######..]", endpoint as "* BT1", modifiers as "MOD C.AG". Legible,
 * derivable, and completely flat: every field had the same weight, so nothing
 * could be read at a glance from across a desk.
 *
 * It also used to carry a WPM readout and the keyboard id. Both were removed;
 * the id's removal takes a ratified procedure with it, and where that went is
 * recorded in the band D block below rather than left to be rediscovered.
 *
 * It now draws vendored 1bpp glyphs (USB / Bluetooth marks, connection state
 * dots, modifier keycaps) and Pixel Operator Mono for the fields that matter,
 * with the batteries as actual drawn gauges. Provenance, licences and the
 * reason none of the upstream projects could be used as a west module are in
 * vendor/README.md -- the short version is that every ZMK display library is a
 * KEYBOARD-LOCAL status screen wired to ZMK event listeners, and this device
 * has no keyboard, so their widgets would render the scanner's own empty state.
 *
 * Two properties of the borrowed assets are what make this cheap rather than a
 * rewrite of the whole rendering strategy:
 *
 *   * The glyphs are already LV_COLOR_FORMAT_I1, the panel's own format, so
 *     they are lv_image objects pointed at a const descriptor. CONFIG_LV_USE_-
 *     IMAGE=y is the entire cost; there is still NO canvas anywhere in this
 *     build, and the batteries are drawn with styled lv_obj rectangles
 *     precisely to keep it that way.
 *   * Pixel Operator Mono has an 8 px advance, exactly like unscii_8. So the
 *     "count the characters" width arithmetic below survives the font swap
 *     unchanged; only the line height moves (9 -> 13).
 * ------------------------------------------------------------------------- */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <lvgl.h>

#include <zmk/display.h>
#include <zmk/display/status_screen.h>

#include "dispscan_status.h"
#include "vendor/dispscan_assets.h"

/* -------------------------------------------------------------------------
 * Geometry
 *
 * Two fonts, both 8 px advance, so a width is still a character count:
 *
 *   dispscan_font_pixel_operator  8x13  headline fields (layer, batteries)
 *   lv_font_unscii_8              8x9   secondary fields (markers, WPM, id)
 *
 * The advance equality is not a coincidence to be relied on loosely -- it is
 * checked in the vendored font's banner against all 95 glyph descriptors
 * (.adv_w = 128, and LVGL stores advance in 1/16 px). 160 px / 8 = 20 columns.
 * ------------------------------------------------------------------------- */
#define PANEL_W 160
#define PANEL_H 68

/* -------------------------------------------------------------------------
 * SAFE AREA -- a uniform inset on all four sides, not just the sides.
 *
 * The horizontal inset used to be 2 px and there was no vertical one at all:
 * band A started at y=0 and the last band ended 5 px from the bottom, so
 * content sat hard against the top edge and the whole composition read as
 * top-weighted. On a panel behind any kind of bezel or case lip, a glyph
 * column or scanline that close to the edge is also the first thing to be
 * cropped.
 *
 * 4 px on every side, and every band position below is derived from it. 160 -
 * 2*4 = 152 px = 19 usable characters per line; 68 - 2*4 = 60 px of usable
 * height against 52 px of content, which is what the inter-band gaps spend.
 * ------------------------------------------------------------------------- */
#define MARGIN 4

/*
 * BAND LAYOUT -- this bounds invalidation HEIGHT. It is not what keeps
 * ls0xx_write() happy; that is already guaranteed elsewhere.
 *
 * ls0xx_write() does reject any `desc->width != LS0XX_PANEL_WIDTH`, but that
 * rejection can never fire here and the band layout is not why. The driver
 * reports `current_pixel_format = PIXEL_FORMAT_MONO01`, so Zephyr's LVGL glue
 * (zephyr/modules/lvgl/lvgl_display.c) registers `lvgl_rounder_cb_mono`, and
 * that callback unconditionally does `area->x1 = 0; area->x2 =
 * cap.x_resolution - 1`. EVERY flush is therefore already full-width before it
 * reaches the driver, whatever shape the invalidated object was. The driver
 * also advertises SCREEN_INFO_X_ALIGNMENT_WIDTH, which is what tells the glue
 * this is required.
 *
 * What the bands buy is the other axis: the mono rounder widens x but leaves y
 * alone, so an object's y extent is exactly the number of scanlines pushed over
 * SPI. Giving each independently-changing field its own y band bounds a
 * single-field update to that band instead of all 68 rows.
 *
 *   y=4   band A  h=14  endpoint icons (left) | layer name+number (right)
 *   y=21  band B  h=13  left battery gauge    | right battery gauge
 *   y=37  band C  h=16  modifier keycaps      | STALE | CAPS
 *   y=55  band D  h=9   (empty)               | RSSI
 *
 * Band A is 14 rather than 13 because the USB/BT marks are 14 px tall and the
 * font is 13; the band is sized by its tallest occupant. Band C is 16: a 14 px
 * keycap plus a 1 px gap plus the 1 px active-underline beneath it.
 *
 * VERTICAL BUDGET, so the spacing is checked rather than eyeballed:
 *   4 top margin + 14 + 3 + 13 + 3 + 16 + 2 + 9 + 4 bottom margin = 68 exactly.
 * The last band ends at 55+9 = 64, leaving the same 4 px at the bottom as at
 * the top. The gaps taper (3, 3, 2) because band C's underline row already
 * reads as a separator and a full gap under it would look like a hole.
 *
 * Bands hold more than one field where those fields tend to change together
 * (both batteries; the two shift-ish markers) -- splitting them would buy no
 * invalidation and cost vertical space, which is the scarce axis here.
 */
#define BAND_A_Y (MARGIN)      /* 4 */
#define BAND_B_Y (MARGIN + 17) /* 21 */
#define BAND_C_Y (MARGIN + 33) /* 37 */
#define BAND_D_Y (MARGIN + 51) /* 55 */

/* -------------------------------------------------------------------------
 * Band A -- endpoint cluster, left.
 *
 * Two groups, each "mark, then state": the USB mark with its state dot, then
 * the Bluetooth mark with its profile digit and state dot. Marks are 9x14,
 * state dots 5x5, the digit is one 8 px character.
 *
 *   x=4    USB mark        9 wide -> 4..12
 *   x=14   USB state dot   5 wide -> 14..18, bottom-aligned (y = +9)
 *   x=24   BT mark         9 wide -> 24..32
 *   x=34   BT profile digit 8 wide -> 34..41
 *   x=43   BT state dot    5 wide -> 43..47, bottom-aligned
 *
 * Cluster ends at x=47. The layer label is right-aligned to the safe area and
 * its worst case ("L255 ABCD", 9 chars = 72 px) therefore spans 84..155, so
 * there is a 36 px gap that nothing can close -- which is also where the FAKE
 * marker lives.
 * ------------------------------------------------------------------------- */
#define EP_USB_X (MARGIN)            /* 4 */
#define EP_USB_STATE_X (MARGIN + 10) /* 14 */
#define EP_BT_X (MARGIN + 20)        /* 24 */
#define EP_BT_DIGIT_X (MARGIN + 30)  /* 34 */
#define EP_BT_STATE_X (MARGIN + 39)  /* 43 */
/* Marks are 14 tall, state dots 5: sit the dot on the mark's baseline. */
#define EP_STATE_DY 9

/* -------------------------------------------------------------------------
 * Band B -- the two battery gauges.
 *
 * A drawn gauge instead of the old "[######..]" cell string. Same information,
 * a third of the width, and readable without counting anything.
 *
 *   x=4    "L"           1 char  ->  4..11
 *   x=14   gauge body    22 wide -> 14..35,  nub 36..37
 *   x=41   percentage    4 chars -> 41..72
 *   x=87   "R"                   -> 87..94
 *   x=97   gauge body            -> 97..118, nub 119..120
 *   x=124  percentage            -> 124..155
 *
 * THE TWO HALVES ARE MIRRORED, which is the alignment property worth stating:
 * the left cluster spans 4..72 and the right 87..155, so each is 69 px wide and
 * each is inset exactly MARGIN from its own side of the panel. The gutter
 * between them (73..86) is centred on the panel midline. A reading is therefore
 * always the same distance from the edge nearest it.
 *
 * (Ranges are inclusive of both ends, so an n-wide object at x spans
 * x .. x+n-1.) The percentage field is 4 wide because "255%" is representable
 * -- see fmt_battery_text on why an impossible reading is shown rather than
 * clamped.
 * ------------------------------------------------------------------------- */
#define BATT_L_SIDE_X (MARGIN)       /* 4 */
#define BATT_L_GAUGE_X (MARGIN + 10) /* 14 */
#define BATT_L_TEXT_X (MARGIN + 37)  /* 41 */
#define BATT_R_SIDE_X (MARGIN + 83)  /* 87 */
#define BATT_R_GAUGE_X (MARGIN + 93) /* 97 */
#define BATT_R_TEXT_X (MARGIN + 120) /* 124 */

#define GAUGE_W 22
#define GAUGE_H 11
/* Interior after the 1 px border, inset a further 1 px so the fill never
 * touches it: 22 - 2*(1+1) = 18 wide, 11 - 2*(1+1) = 7 tall. */
#define GAUGE_FILL_MAX_W 18
#define GAUGE_FILL_H 7
#define GAUGE_FILL_INSET 2
/* The terminal nub, on the right-hand end, so the shape reads as a battery
 * rather than a progress bar. */
#define GAUGE_NUB_W 2
#define GAUGE_NUB_H 5
/* Gauges are 11 tall inside a 13 px band -- centre them. */
#define GAUGE_DY 1

/* -------------------------------------------------------------------------
 * Band C -- modifier keycaps, and the two shift-ish markers.
 *
 * Four 14x14 keycaps on an 18 px pitch, one per modifier CLASS (left/right are
 * not on the wire -- dispscan_status.h, trap #4):
 *
 *   x=4, 22, 40, 58  ->  CTRL, SHIFT, ALT, GUI, last ends at 71
 *
 * ALWAYS VISIBLE, with a 1 px underline appearing beneath the held ones. This
 * is upstream's idiom and it preserves the property the old "MOD C.AG" string
 * had for the same stated reason: fixed columns, so the eye reads a POSITION
 * rather than parsing a list, and the string never changes width. It is not in
 * tension with the blank-unless-set rule applied to CAPS and STALE below --
 * those are rare, transient, whole-field events with no column of their own.
 *
 *   x=78   STALE  5 chars = 40 px -> 78..117
 *   right  CAPS   4 chars = 32 px -> 124..155
 *
 * Evenly gapped: 7 px from the keycaps to STALE, 7 px from STALE to CAPS, and
 * CAPS lands on the safe-area edge like every other right-aligned field.
 * ------------------------------------------------------------------------- */
#define MOD_COUNT 4
#define MOD_ICON_W 14
#define MOD_ICON_H 14
#define MOD_PITCH 18
#define MOD_FIRST_X (MARGIN) /* 4 */
/* Underline sits 1 px under the keycap and is as wide as it. */
#define MOD_UNDERLINE_DY (MOD_ICON_H + 1)
#define MOD_UNDERLINE_H 1

/* The markers use unscii_8 (9 px) inside a 16 px band; nudge them down so they
 * sit against the keycaps' optical centre rather than their top edge. */
#define MARKER_DY 4
#define STALE_MARKER_X (MARGIN + 74) /* 78 */
#define STALE_MARKER_TEXT "STALE"

/* -------------------------------------------------------------------------
 * Band D -- RSSI, right-aligned, and nothing else.
 *
 *   right  "-128dBm"  worst case, 7 chars = 56 px -> 99..155
 *
 * WHAT USED TO BE HERE, AND WHY IT IS NOT:
 *
 *   WPM. Removed on request. The field is still decoded and still crosses the
 *   seam (dispscan_status.h) -- it is simply not drawn. Nothing else changes:
 *   the wire carries it whether or not this file reads it.
 *
 *   THE KEYBOARD ID. Removed on request, and this one has a consequence worth
 *   recording, because it silently breaks a ratified procedure if nobody
 *   writes it down. D8's binding step was "read the 8 hex digits off the panel
 *   and paste them into CONFIG_DISPSCAN_KEYBOARD_ID_ALLOWLIST". With the field
 *   gone the panel can no longer tell you the id.
 *
 *   THE REPLACEMENT IS THE SERIAL LOG, which already carried it and needed no
 *   new code: dispscan_observer.c logs `bound to keyboard_id %08X` at LOG_INF
 *   the moment it binds, and CONFIG_ZMK_USB_LOGGING is on. So binding is now
 *   "plug the display into USB, watch the console, copy the id from there".
 *   If USB logging is ever turned off (it is marked DEV ONLY in dispscan.conf)
 *   this procedure loses its last route and the field has to come back.
 *
 * The band is otherwise empty. It is kept rather than reclaimed because the
 * alternative -- pulling the other three bands down into the freed rows --
 * would spread 43 px of content over 60 px of safe area, and the gaps would
 * then be wider than the 13 px band heights they separate.
 * ------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------
 * Glyph coverage
 *
 * Both faces cover ASCII 0x20..0x7E and nothing else. Any other byte renders as
 * LVGL's missing-glyph box (or nothing), which both breaks the
 * character-counting width arithmetic above and hides the fact that the input
 * was malformed.
 *
 * Substituting a visible '?' makes both problems obvious instead: the width is
 * preserved exactly (one byte in, one 8 px cell out) and the eye sees that the
 * keyboard sent something the panel cannot represent.
 * ------------------------------------------------------------------------- */
#define GLYPH_MIN 0x20
#define GLYPH_MAX 0x7E
#define GLYPH_SUBST '?'

/* -------------------------------------------------------------------------
 * Colour -- LVGL COLOUR CONSTANTS ARE INVERTED RELATIVE TO THE PANEL
 *
 * CONFIRMED ON HARDWARE 2026-08-07: lv_color_white() renders BLACK on the
 * nice!view, and lv_color_black() renders WHITE. Do not "fix" the names below
 * to read more naturally -- they are correct as written.
 *
 * The chain, all verified in the pinned trees:
 *   1. LVGL at LV_COLOR_FORMAT_I1 maps white to bit 1, black to bit 0.
 *   2. ls0xx reports current_pixel_format = PIXEL_FORMAT_MONO01, and Zephyr's
 *      lvgl_display_mono.c set_px_at_pos() CLEARS the destination bit for a set
 *      source pixel in the MONO01 case (`*buf &= ~BIT(bit)`), starting from a
 *      0xFF-filled buffer.
 *   3. On a Sharp memory LCD a 0 bit is a BLACK pixel.
 * Net: LVGL white -> panel black.
 *
 * ZMK's own nice_view widgets encode the same inversion, which is the clearest
 * confirmation that this is expected rather than a misconfiguration here:
 * widgets/util.h defines LVGL_FOREGROUND as lv_color_black() in the
 * non-inverted case, and widgets/util.c then passes it to a descriptor it names
 * `rect_white_dsc`. ZMK names those by what appears on the glass, not by the
 * constant. Hence the stock nice!view look -- light content on a dark field --
 * which is also what this screen produces.
 *
 * The vendored glyphs agree with this by construction; see the palette note in
 * vendor/dispscan_assets.h. No recolouring is applied to any icon.
 *
 * DARK therefore uses lv_color_white(). docs/remote-display-plan.md, "Rendering
 * \"dark\"", is explicit: *"Fill black -- white reads as a dead panel."* A
 * bright frame on a reflective LCD reads as broken, which is exactly the wrong
 * signal for a state meaning "the keyboard is fine, it is just idle". Because
 * DARK's panel-black equals the normal background, DARK is simply the ordinary
 * field with every label hidden.
 *
 * Either way it is a DRAWN frame. display_blanking_on() must never be used
 * here: Zephyr's ls0xx.c only compiles its blanking hooks under `disp_en_gpios`,
 * which the nice!view does not expose, so the call is a silent no-op and the
 * panel would keep showing the last -- now stale -- frame forever.
 * ------------------------------------------------------------------------- */
#define COL_BG lv_color_white()   /* -> panel BLACK: the field */
#define COL_FG lv_color_black()   /* -> panel WHITE: the content */
#define COL_DARK lv_color_white() /* -> panel BLACK: fully extinguished */

/* -------------------------------------------------------------------------
 * Which keycap art stands for GUI and ALT.
 *
 * The wire carries modifier CLASSES, not key legends, so this is purely a
 * question of what the person reading the panel expects to see. Mirrors
 * upstream's ZMK_DONGLE_DISPLAY_MAC_MODIFIERS, and DEFAULTS TO THE MAC GLYPHS
 * here (option and command) because that is what this keyboard is driven from.
 *
 * Note the keyboard itself already has conditional layers keyed on the host OS
 * (config/charybdis.keymap on the keyboard branches), so a Windows session is
 * not hypothetical -- but the modifier CLASS is identical either way, and only
 * the legend differs. Flip CONFIG_DISPSCAN_MAC_MODIFIERS=n to get the Windows
 * keycaps back.
 * ------------------------------------------------------------------------- */
#if IS_ENABLED(CONFIG_DISPSCAN_MAC_MODIFIERS)
#define MOD_ICON_ALT (&dispscan_opt_icon)
#define MOD_ICON_GUI (&dispscan_cmd_icon)
#else
#define MOD_ICON_ALT (&dispscan_alt_icon)
#define MOD_ICON_GUI (&dispscan_win_icon)
#endif

/* -------------------------------------------------------------------------
 * Object tree -- built ONCE in zmk_display_status_screen(), mutated thereafter.
 *
 * Rebuilding the tree per update would allocate out of LV_Z_MEM_POOL_SIZE on
 * every beacon and invalidate the whole screen each time, defeating the band
 * layout above.
 * ------------------------------------------------------------------------- */
struct dispscan_ui {
    bool built;

    lv_obj_t *screen;

    /* Band A -- endpoint cluster and layer. */
    lv_obj_t *img_usb;
    lv_obj_t *img_usb_state;
    lv_obj_t *img_bt;
    lv_obj_t *lbl_bt_digit;
    lv_obj_t *img_bt_state;
    lv_obj_t *lbl_layer;

    /* Band B -- batteries. Three objects per gauge: the bordered body, the fill
     * inside it, and the terminal nub. */
    lv_obj_t *lbl_batt_l_side;
    lv_obj_t *gauge_l_body;
    lv_obj_t *gauge_l_fill;
    lv_obj_t *gauge_l_nub;
    lv_obj_t *lbl_batt_l_text;
    lv_obj_t *lbl_batt_r_side;
    lv_obj_t *gauge_r_body;
    lv_obj_t *gauge_r_fill;
    lv_obj_t *gauge_r_nub;
    lv_obj_t *lbl_batt_r_text;

    /* Band C -- modifiers and markers. */
    lv_obj_t *img_mod[MOD_COUNT];
    lv_obj_t *mod_underline[MOD_COUNT];
    lv_obj_t *lbl_stale;
    lv_obj_t *lbl_caps;

    /* Band D. */
    lv_obj_t *lbl_rssi;

    /* NO_SIGNAL composition. */
    lv_obj_t *lbl_nosig;
    lv_obj_t *lbl_nosig_sub;

#ifdef CONFIG_DISPSCAN_FAKE_SOURCE
    /* Synthetic-data marker. Visible in EVERY state -- see FAKE_MARKER_X. */
    lv_obj_t *lbl_fake;
#endif

    /* Last state actually painted, so an update that changes one field repaints
     * one band. `has_last` is false until the first paint. */
    bool has_last;
    struct dispscan_status last;
};

static struct dispscan_ui ui;

/* Both defined further down; zmk_display_status_screen() needs them to replay a
 * pre-init update as the last step of construction (see dispscan_status.h's
 * contract) and C requires the declaration first. */
static void render(const struct dispscan_status *s);
static bool peek_pending(struct dispscan_status *out);

#ifdef CONFIG_DISPSCAN_FAKE_SOURCE
/*
 * SYNTHETIC-DATA MARKER.
 *
 * With CONFIG_DISPSCAN_FAKE_SOURCE=y the panel shows a completely convincing
 * live keyboard that is in fact a timer emitting made-up numbers. The only
 * other warning is a LOG_WRN on a USB console that may not be attached, so a
 * photograph of this device -- or the device itself a week later -- is
 * indistinguishable from working hardware receiving real packets. Put it on the
 * glass.
 *
 * GEOMETRY. "FAKE" is 4 chars = 32 px in unscii_8, left-aligned at the safe
 * margin in BAND D, spanning x=4..35. It lives there because band D's left half
 * fell vacant when WPM was removed: it is the only genuinely empty space on the
 * panel, so the marker needs no gap negotiated against anything and nothing
 * moves to accommodate it. The RSSI it shares the band with is right-aligned
 * and starts no earlier than x=99.
 *
 * It used to sit in band A, squeezed between the endpoint cluster and the
 * layer label. That gap is now 36 px against a 32 px marker -- it would still
 * fit, with 2 px either side, but a 2 px clearance is the kind of margin that
 * breaks the next time any band A field grows.
 *
 * Band D also clears the NO_SIGNAL text, which is what the old placement was
 * really buying: those two centred lines occupy roughly y=20..46, and band D
 * starts at y=55.
 *
 * DELIBERATELY NOT IN collect_awake_objs(). It must stay visible in DARK and
 * NO_SIGNAL too -- those are precisely the states that would otherwise look
 * like a real keyboard idling or out of range.
 */
#define FAKE_MARKER_X (MARGIN)
#define FAKE_MARKER_TEXT "FAKE"
#endif /* CONFIG_DISPSCAN_FAKE_SOURCE */

/* -------------------------------------------------------------------------
 * NULL tolerance
 *
 * EVERY object pointer in `ui` is allowed to be NULL: the make_* helpers return
 * NULL when LVGL's pool is exhausted rather than letting the caller fault.
 * Routing every mutation through these wrappers is what turns "undersized
 * LV_Z_MEM_POOL_SIZE" from a hard fault inside the boot-time status-screen
 * constructor (blank panel, no serial, no diagnostic) into a partially-drawn
 * screen plus a LOG_ERR.
 * ------------------------------------------------------------------------- */

static void set_hidden(lv_obj_t *obj, bool hidden) {
    if (obj == NULL) {
        return;
    }
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_text(lv_obj_t *obj, const char *txt) {
    if (obj == NULL) {
        return;
    }
    lv_label_set_text(obj, txt);
}

static void set_img(lv_obj_t *obj, const lv_img_dsc_t *src) {
    if (obj == NULL) {
        return;
    }
    lv_image_set_src(obj, src);
}

static void set_width(lv_obj_t *obj, int32_t w) {
    if (obj == NULL) {
        return;
    }
    lv_obj_set_width(obj, w);
}

#ifdef CONFIG_DISPSCAN_FAKE_SOURCE
/* The marker is never hidden, in any state, including DARK.
 *
 * It used to flip colour in DARK, back when COL_DARK was believed to paint a
 * bright frame. It does not -- see the colour block above; DARK's field is the
 * same panel-black as COL_BG -- so the content colour is COL_FG throughout and
 * the flip would have made the marker invisible, which is the precise failure
 * the marker exists to prevent.
 */
static void fake_marker_set_dark(bool dark) {
    ARG_UNUSED(dark);
    if (ui.lbl_fake == NULL) {
        return;
    }
    lv_obj_set_style_text_color(ui.lbl_fake, COL_FG, LV_PART_MAIN);
}
#define FAKE_MARKER_SET_DARK(d) fake_marker_set_dark(d)
#else
#define FAKE_MARKER_SET_DARK(d) ((void)0)
#endif

/*
 * Everything visible only in AWAKE, so state switching is one loop. Filled on
 * each call rather than cached because the pointers are only valid after
 * construction and this is off the hot path (state transitions only).
 *
 * The count is asserted against the writes below rather than maintained by
 * hand: a field added to the composition without a slot here would silently
 * stay on screen through DARK and NO_SIGNAL, which is the exact class of bug
 * this screen exists to avoid.
 */
#define AWAKE_OBJ_COUNT (16 + 2 * MOD_COUNT)

static size_t collect_awake_objs(lv_obj_t *objs[AWAKE_OBJ_COUNT]) {
    size_t n = 0;

    objs[n++] = ui.img_usb;
    objs[n++] = ui.img_usb_state;
    objs[n++] = ui.img_bt;
    objs[n++] = ui.lbl_bt_digit;
    objs[n++] = ui.img_bt_state;
    objs[n++] = ui.lbl_layer;

    objs[n++] = ui.lbl_batt_l_side;
    objs[n++] = ui.gauge_l_body;
    objs[n++] = ui.gauge_l_fill;
    objs[n++] = ui.gauge_l_nub;
    objs[n++] = ui.lbl_batt_l_text;
    objs[n++] = ui.lbl_batt_r_side;
    objs[n++] = ui.gauge_r_body;
    objs[n++] = ui.gauge_r_fill;
    objs[n++] = ui.gauge_r_nub;
    objs[n++] = ui.lbl_batt_r_text;

    for (int i = 0; i < MOD_COUNT; i++) {
        objs[n++] = ui.img_mod[i];
        objs[n++] = ui.mod_underline[i];
    }

    /* DELIBERATELY NOT HERE: lbl_stale, lbl_caps and lbl_rssi.
     * set_awake_hidden() handles those three separately because two of them are
     * conditional even within AWAKE -- see the comment there. */
    __ASSERT(n == AWAKE_OBJ_COUNT, "awake object count drifted");
    return n;
}

static void set_awake_hidden(bool hidden) {
    lv_obj_t *objs[AWAKE_OBJ_COUNT];
    size_t n = collect_awake_objs(objs);

    for (size_t i = 0; i < n; i++) {
        set_hidden(objs[i], hidden);
    }

    /* STALE and CAPS are in the AWAKE set on purpose but are not in the array:
     * each has its own show/hide rule inside render_awake() (blank unless the
     * condition holds), so they are only ever FORCED hidden here, never forced
     * visible. Showing them on the way back into AWAKE would resurrect a marker
     * that may no longer apply; render_awake(force=true) sets them correctly a
     * moment later. */
    if (hidden) {
        set_hidden(ui.lbl_stale, true);
        set_hidden(ui.lbl_caps, true);
    }
    set_hidden(ui.lbl_rssi, hidden);
}

/* -------------------------------------------------------------------------
 * Formatting
 *
 * snprintf into a local buffer, then lv_label_set_text -- deliberately NOT
 * lv_label_set_text_fmt, which routes through LVGL's own vsnprintf shim and
 * would add a dependency on LV_SPRINTF_* config this build otherwise never
 * touches.
 * ------------------------------------------------------------------------- */

/*
 * The battery reading, as text: " 87%" / "  NA" / "255%".
 *
 * Fixed 4-character field so the two gauges' labels align and the worst case is
 * known. Paired with set_gauge() below, which draws the bar. The three cases
 * are kept apart on BOTH channels -- outline and text -- rather than relying on
 * either alone:
 *
 *   pct == 0    N/A per the wire contract, NOT an empty battery. Empty outline,
 *               text "NA".
 *   1..100      outline plus a fill, floored at 1 px so a nearly-flat half
 *               never renders as the empty bar that means N/A.
 *   pct > 100   impossible per the wire contract. NO outline at all, and the
 *               text shows the REAL value.
 *
 * N/A KEEPS ITS OUTLINE, and that is a correction. It was originally drawn with
 * no outline, on the theory that "a gauge exists iff the value is legal" was
 * the cleanest invariant. On screen it is not: a missing gauge leaves an
 * obvious hole in band B, which reads as a broken display rather than as an
 * absent reading -- and a left half reporting N/A is a routine state here, not
 * an exotic one. An empty cell next to the word "NA" says "no reading" without
 * implying the panel itself has failed. The out-of-range case keeps the missing
 * outline, where "something is wrong" is exactly the intended message.
 *
 * THE OUT-OF-RANGE VALUE IS NOT CLAMPED, deliberately. It used to be, with the
 * stated justification that clamping avoided widening the field -- which was
 * factually false, since "%3u%%" of 255 is exactly the 4 characters already
 * budgeted. Battery is also the field where laundering is most dangerous: a
 * one-byte offset error in the decoder would clamp two garbage bytes into two
 * healthy-looking batteries, the most reassuring possible rendering of a
 * completely broken decode. Same rule fmt_link_digit() applies to the profile.
 */
static void fmt_battery_text(char *out, size_t out_len, uint8_t pct) {
    if (pct == 0) {
        snprintf(out, out_len, "  NA");
    } else {
        /* %3u%% keeps the value column exactly 4 wide across 1..255. */
        snprintf(out, out_len, "%3u%%", (unsigned int)pct);
    }
}

/*
 * The battery reading, as pixels. See the three cases in fmt_battery_text().
 *
 * Width is set rather than the object recreated: an lv_obj_set_width() on a
 * child inside band B invalidates those 13 scanlines and nothing else.
 */
static void set_gauge(lv_obj_t *body, lv_obj_t *fill, lv_obj_t *nub, uint8_t pct) {
    /* The outline stands for "this is a battery reading", which N/A still is --
     * it is a reading of "none available". Only an impossible value retracts
     * the claim entirely. */
    bool drawable = (pct <= 100);

    set_hidden(body, !drawable);
    set_hidden(nub, !drawable);
    /* The fill is what carries the LEVEL, so it goes away at 0 even though the
     * outline stays. */
    set_hidden(fill, !drawable || pct == 0);

    if (!drawable || pct == 0) {
        return;
    }

    /* Round to nearest pixel column, then floor at one. */
    int w = (pct * GAUGE_FILL_MAX_W + 50) / 100;

    if (w < 1) {
        w = 1;
    }
    set_width(fill, w);
}

/*
 * "L2 NAV" -- name preferred, number always present so a blank name still
 * identifies the layer. Worst case is "L255 ABCD" = 9 chars = 72 px; the label
 * is right-aligned, so that case starts at x=86 and clears the endpoint cluster
 * (which ends at 46) by 40 px.
 */
static void fmt_layer(char *out, size_t out_len, const struct dispscan_status *s) {
    /*
     * SANITISE FIRST. The decoded contract guarantees NUL-termination and
     * nothing else about the CONTENT: zmk_keymap_layer_name() returns arbitrary
     * user text, and the 4-byte fixed wire field truncates without regard for
     * encoding -- a layer called "Nav->" written with a multi-byte arrow ships
     * as 4E 61 76 E2, a UTF-8 lead byte with its continuation bytes cut off.
     * Neither face has a glyph above 0x7E, so an unsanitised name both renders
     * as garbage and breaks the 8 px-per-character width arithmetic this whole
     * layout rests on.
     */
    char name[DISPSCAN_LAYER_NAME_BUF_LEN];
    size_t i;

    for (i = 0; i < DISPSCAN_LAYER_NAME_WIRE_LEN && s->layer_name[i] != '\0'; i++) {
        unsigned char c = (unsigned char)s->layer_name[i];

        name[i] = (c >= GLYPH_MIN && c <= GLYPH_MAX) ? (char)c : GLYPH_SUBST;
    }
    name[i] = '\0';

    /* uint8_t promotes to int, so every %u below takes an explicit cast rather
     * than relying on int/unsigned being interchangeable at a varargs boundary. */
    if (name[0] != '\0') {
        snprintf(out, out_len, "L%u %s", (unsigned int)s->active_layer, name);
    } else {
        snprintf(out, out_len, "L%u", (unsigned int)s->active_layer);
    }
}

/*
 * The BLE profile slot, as one character next to the Bluetooth mark.
 *
 * profile_slot arrives already masked & 0x07 (trap #1), so a value above
 * DISPSCAN_PROFILE_MAX means the keyboard reported a slot outside its profile
 * count -- shown as '?' rather than silently clamped, because that would be a
 * decoder bug worth seeing on the panel.
 *
 * ZERO-BASED, matching the wire and ZMK's own numbering. Upstream's icon set
 * renders the index one-based (its sym_1 is profile 0); those glyphs were
 * dropped rather than adopt a second numbering for the same field -- see
 * vendor/README.md.
 */
static void fmt_link_digit(char *out, size_t out_len, const struct dispscan_status *s) {
    if (s->profile_slot <= DISPSCAN_PROFILE_MAX) {
        snprintf(out, out_len, "%u", (unsigned int)s->profile_slot);
    } else {
        snprintf(out, out_len, "?");
    }
}

/*
 * Endpoint state dots. Three states per transport, and every one of them is
 * information the old single-character rendering threw away or conflated:
 *
 *   USB   ok    HID ready -- the host is taking keystrokes over the cable
 *         nok   cable present, HID not ready (enumerating, or host asleep)
 *         open  no cable
 *   BT    ok    connected AND bonded
 *         nok   connected, not bonded (a Just Works session)
 *         open  nothing connected; the profile shown is merely the selected one
 *
 * The old "U / * / + / -" scheme could only describe ONE transport at a time,
 * so `usb_connected` was decoded and never drawn: a cable that was plugged in
 * but not yet carrying HID looked identical to no cable at all.
 */
static const lv_img_dsc_t *usb_state_icon(const struct dispscan_status *s) {
    if (s->usb_hid_ready) {
        return &dispscan_sym_ok;
    }
    return s->usb_connected ? &dispscan_sym_nok : &dispscan_sym_open;
}

static const lv_img_dsc_t *bt_state_icon(const struct dispscan_status *s) {
    if (!s->ble_connected) {
        return &dispscan_sym_open;
    }
    return s->ble_bonded ? &dispscan_sym_ok : &dispscan_sym_nok;
}

/* -------------------------------------------------------------------------
 * Construction
 * ------------------------------------------------------------------------- */

/*
 * FAIL LOUDLY AND SURVIVABLY, never fault. Every creator below allocates out of
 * LV_Z_MEM_POOL_SIZE and returns NULL when that pool is exhausted; every style
 * call would then dereference it. Faulting HERE is the worst possible place for
 * it: this runs inside the boot-time status screen constructor, long before USB
 * CDC has enumerated, so the symptom would be a blank panel with no serial
 * output and no diagnostic whatsoever.
 *
 * Returning NULL instead costs one object and keeps the rest of the screen --
 * every mutator above tolerates NULL -- so the failure degrades to "one field
 * is missing" plus a log line that survives to the next boot with a console
 * attached.
 */
static void log_alloc_failure(const char *what, int32_t x, int32_t y) {
    LOG_ERR("dispscan: %s create failed at (%d,%d) -- LVGL pool exhausted "
            "(CONFIG_LV_Z_MEM_POOL_SIZE=%d). This field will not be drawn.",
            what, (int)x, (int)y, (int)CONFIG_LV_Z_MEM_POOL_SIZE);
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_align_t align, int32_t x,
                            int32_t y) {
    lv_obj_t *lbl = lv_label_create(parent);

    if (lbl == NULL) {
        log_alloc_failure("label", x, y);
        return NULL;
    }

    /*
     * The font is set per-object rather than relying on the theme: ZMK only
     * installs a theme under CONFIG_LV_USE_THEME_MONO, which this build does not
     * enable (app/src/display/Kconfig only `imply`s it for the BUILT_IN status
     * screen, and this shield selects the CUSTOM one). Without a theme an object
     * inherits no font, so this is mandatory rather than stylistic.
     */
    lv_obj_set_style_text_font(lbl, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, COL_FG, LV_PART_MAIN);
    lv_label_set_text(lbl, "");
    lv_obj_align(lbl, align, x, y);
    return lbl;
}

static lv_obj_t *make_image(lv_obj_t *parent, const lv_img_dsc_t *src, int32_t x, int32_t y) {
    lv_obj_t *img = lv_image_create(parent);

    if (img == NULL) {
        log_alloc_failure("image", x, y);
        return NULL;
    }

    lv_image_set_src(img, src);
    lv_obj_align(img, LV_ALIGN_TOP_LEFT, x, y);
    return img;
}

/*
 * A filled rectangle. Used for the gauge fills, the gauge nubs and the modifier
 * underlines -- i.e. every solid block on this screen.
 *
 * lv_obj rather than lv_canvas or lv_line on purpose: a styled lv_obj is a core
 * object with no extra LV_USE_* symbol behind it, so the whole icon-and-gauge
 * composition still costs exactly one config change (LV_USE_IMAGE) over the
 * text-only screen it replaces.
 */
static lv_obj_t *make_block(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h) {
    lv_obj_t *obj = lv_obj_create(parent);

    if (obj == NULL) {
        log_alloc_failure("block", x, y);
        return NULL;
    }

    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, COL_FG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_align(obj, LV_ALIGN_TOP_LEFT, x, y);
    return obj;
}

/* The gauge outline: a 1 px border with the field showing through. */
static lv_obj_t *make_gauge_body(lv_obj_t *parent, int32_t x, int32_t y) {
    lv_obj_t *obj = lv_obj_create(parent);

    if (obj == NULL) {
        log_alloc_failure("gauge", x, y);
        return NULL;
    }

    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, GAUGE_W, GAUGE_H);
    lv_obj_set_style_bg_color(obj, COL_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, COL_FG, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN);
    lv_obj_align(obj, LV_ALIGN_TOP_LEFT, x, y);
    return obj;
}

/* One battery: side letter, gauge body, fill, nub, percentage text. */
static void build_battery(lv_obj_t *parent, const char *side, int32_t side_x, int32_t gauge_x,
                          int32_t text_x, lv_obj_t **lbl_side, lv_obj_t **body, lv_obj_t **fill,
                          lv_obj_t **nub, lv_obj_t **lbl_text) {
    *lbl_side = make_label(parent, &dispscan_font_pixel_operator, LV_ALIGN_TOP_LEFT, side_x,
                           BAND_B_Y);
    set_text(*lbl_side, side);

    *body = make_gauge_body(parent, gauge_x, BAND_B_Y + GAUGE_DY);
    /* The fill is a child of the body, so its coordinates are relative to it and
     * the pair moves together if the layout is ever nudged. */
    *fill = (*body != NULL) ? make_block(*body, GAUGE_FILL_INSET - 1, GAUGE_FILL_INSET - 1,
                                         GAUGE_FILL_MAX_W, GAUGE_FILL_H)
                            : NULL;
    *nub = make_block(parent, gauge_x + GAUGE_W, BAND_B_Y + GAUGE_DY + (GAUGE_H - GAUGE_NUB_H) / 2,
                      GAUGE_NUB_W, GAUGE_NUB_H);

    *lbl_text = make_label(parent, &dispscan_font_pixel_operator, LV_ALIGN_TOP_LEFT, text_x,
                           BAND_B_Y);
}

/*
 * Signature intentionally matches include/zmk/display/status_screen.h exactly
 * (empty parameter list, not `void`), so the weak-symbol override binds.
 *
 * Runs on the display work queue: app/src/display/main.c calls this from
 * initialize_display(), which is itself a k_work submitted to
 * zmk_display_work_q() (main.c:161). So it is already serialised against
 * update_work_cb() below.
 */
lv_obj_t *zmk_display_status_screen() {
    struct dispscan_status replay;
    static const lv_img_dsc_t *const mod_icons[MOD_COUNT] = {
        &dispscan_control_icon,
        &dispscan_shift_icon,
        MOD_ICON_ALT,
        MOD_ICON_GUI,
    };

    ui.screen = lv_obj_create(NULL);

    /*
     * Same rule as the make_* helpers: no unchecked dereference in the boot
     * path. If even the root object cannot be allocated there is no screen to
     * build, so return NULL -- app/src/display/main.c handles that explicitly
     * (`if (screen == NULL) { LOG_ERR("No status screen provided"); return; }`),
     * leaving a running device with a logged reason instead of a fault.
     */
    if (ui.screen == NULL) {
        LOG_ERR("dispscan: lv_obj_create failed for the root screen -- LVGL pool "
                "exhausted (CONFIG_LV_Z_MEM_POOL_SIZE=%d). No status screen.",
                (int)CONFIG_LV_Z_MEM_POOL_SIZE);
        return NULL;
    }

    lv_obj_set_style_bg_color(ui.screen, COL_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui.screen, LV_OPA_COVER, LV_PART_MAIN);
    /* No scrollbars, no elastic scroll: the panel is exactly one screen, and a
     * scrollbar would be an animating object -- i.e. a permanent repaint. */
    lv_obj_set_scrollbar_mode(ui.screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(ui.screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(ui.screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui.screen, 0, LV_PART_MAIN);

    /* Band A. */
    ui.img_usb = make_image(ui.screen, &dispscan_sym_usb, EP_USB_X, BAND_A_Y);
    ui.img_usb_state = make_image(ui.screen, &dispscan_sym_open, EP_USB_STATE_X,
                                  BAND_A_Y + EP_STATE_DY);
    ui.img_bt = make_image(ui.screen, &dispscan_sym_bt, EP_BT_X, BAND_A_Y);
    ui.lbl_bt_digit = make_label(ui.screen, &dispscan_font_pixel_operator, LV_ALIGN_TOP_LEFT,
                                 EP_BT_DIGIT_X, BAND_A_Y);
    ui.img_bt_state = make_image(ui.screen, &dispscan_sym_open, EP_BT_STATE_X,
                                 BAND_A_Y + EP_STATE_DY);
    ui.lbl_layer = make_label(ui.screen, &dispscan_font_pixel_operator, LV_ALIGN_TOP_RIGHT, -MARGIN,
                              BAND_A_Y);

    /* Band B. */
    build_battery(ui.screen, "L", BATT_L_SIDE_X, BATT_L_GAUGE_X, BATT_L_TEXT_X, &ui.lbl_batt_l_side,
                  &ui.gauge_l_body, &ui.gauge_l_fill, &ui.gauge_l_nub, &ui.lbl_batt_l_text);
    build_battery(ui.screen, "R", BATT_R_SIDE_X, BATT_R_GAUGE_X, BATT_R_TEXT_X, &ui.lbl_batt_r_side,
                  &ui.gauge_r_body, &ui.gauge_r_fill, &ui.gauge_r_nub, &ui.lbl_batt_r_text);

    /* Band C. */
    for (int i = 0; i < MOD_COUNT; i++) {
        int32_t x = MOD_FIRST_X + i * MOD_PITCH;

        ui.img_mod[i] = make_image(ui.screen, mod_icons[i], x, BAND_C_Y);
        ui.mod_underline[i] =
            make_block(ui.screen, x, BAND_C_Y + MOD_UNDERLINE_DY, MOD_ICON_W, MOD_UNDERLINE_H);
        /* Underlines start off; render_awake() turns on the held ones. */
        set_hidden(ui.mod_underline[i], true);
    }
    ui.lbl_stale = make_label(ui.screen, &lv_font_unscii_8, LV_ALIGN_TOP_LEFT, STALE_MARKER_X,
                              BAND_C_Y + MARKER_DY);
    ui.lbl_caps = make_label(ui.screen, &lv_font_unscii_8, LV_ALIGN_TOP_RIGHT, -MARGIN,
                             BAND_C_Y + MARKER_DY);

    /* Band D. */
    ui.lbl_rssi = make_label(ui.screen, &lv_font_unscii_8, LV_ALIGN_TOP_RIGHT, -MARGIN, BAND_D_Y);

    /*
     * NO_SIGNAL composition. Deliberately the OPPOSITE of DARK: DARK is an
     * all-black frame with nothing on it, NO_SIGNAL is the normal field carrying
     * two centred lines. The two can never be confused, which matters because
     * they mean very different things ("keyboard idle, data still good" vs
     * "keyboard gone, data meaningless").
     *
     * "-- NO SIGNAL --" is 15 chars = 120 px, centred with 20 px either side --
     * comfortably inside the safe area, which is the widest single string on
     * the panel and therefore the one that would clip first.
     *
     * The pair is centred on the PANEL, not on the safe area: LV_ALIGN_CENTER
     * with a symmetric -8/+8 split puts the 13 px headline at y=20..32 and the
     * 9 px subtitle at y=38..46, so the block's optical centre lands on the
     * panel midline (34) with 20 px of field above and 21 below. Insetting it
     * would move a centred block off centre for no gain -- MARGIN exists to
     * keep content off the EDGES, and nothing here is near one.
     */
    ui.lbl_nosig = make_label(ui.screen, &dispscan_font_pixel_operator, LV_ALIGN_CENTER, 0, -8);
    set_text(ui.lbl_nosig, "-- NO SIGNAL --");
    ui.lbl_nosig_sub = make_label(ui.screen, &lv_font_unscii_8, LV_ALIGN_CENTER, 0, 8);
    set_text(ui.lbl_nosig_sub, "no beacon heard");

#ifdef CONFIG_DISPSCAN_FAKE_SOURCE
    /* See FAKE_MARKER_X above for the geometry derivation and for why this one
     * label is exempt from every hide/show path. */
    ui.lbl_fake = make_label(ui.screen, &lv_font_unscii_8, LV_ALIGN_TOP_LEFT, FAKE_MARKER_X,
                             BAND_D_Y);
    set_text(ui.lbl_fake, FAKE_MARKER_TEXT);
#endif

    ui.built = true;
    ui.has_last = false;

    /*
     * Boot into NO_SIGNAL, not AWAKE. Nothing has been heard yet and that is
     * exactly what NO_SIGNAL means; a zeroed AWAKE screen would be a panel full
     * of plausible-looking lies (0% batteries, layer 0, profile 0).
     */
    set_awake_hidden(true);

    LOG_INF("dispscan: status screen built (%dx%d landscape, no rotation, %d px safe area)",
            PANEL_W, PANEL_H, MARGIN);

    /*
     * REPLAY THE PRE-INIT UPDATE. dispscan_status.h promises that
     * dispscan_status_update() is safe to call before the display exists and
     * that the stored update is painted once the screen is built. Without this
     * block that promise was simply false: the update stored `pending` and
     * returned without submitting (the queue did not exist yet), update_work_cb
     * dropped anything arriving before `ui.built`, and nothing ever read
     * `pending` again -- so a producer that pushed exactly once before init was
     * silently orphaned and the panel sat on NO SIGNAL forever.
     *
     * This is not hypothetical for the BLE observer: a real observer may
     * suppress duplicate payloads, so a keyboard whose state is not changing
     * legitimately produces ONE update and then nothing for minutes.
     *
     * Safe to render here: this function already runs on the display work queue
     * (main.c submits initialize_display() to it), which is the same context
     * update_work_cb() uses, so there is no concurrent LVGL access.
     */
    if (peek_pending(&replay)) {
        LOG_INF("dispscan: replaying status update stored before display init");
        render(&replay);
    }

    return ui.screen;
}

/* -------------------------------------------------------------------------
 * Painting
 * ------------------------------------------------------------------------- */

/* @param force repaint every band regardless of the diff -- used on the first
 * paint and whenever the AWAKE objects were hidden and are coming back, since
 * their contents may predate the DARK/NO_SIGNAL excursion. */
static void render_awake(const struct dispscan_status *s, bool force) {
    char buf[24];

    if (force || s->active_layer != ui.last.active_layer ||
        strcmp(s->layer_name, ui.last.layer_name) != 0) {
        fmt_layer(buf, sizeof(buf), s);
        set_text(ui.lbl_layer, buf);
    }

    if (force || s->usb_connected != ui.last.usb_connected ||
        s->usb_hid_ready != ui.last.usb_hid_ready) {
        set_img(ui.img_usb_state, usb_state_icon(s));
    }

    if (force || s->ble_connected != ui.last.ble_connected ||
        s->ble_bonded != ui.last.ble_bonded) {
        set_img(ui.img_bt_state, bt_state_icon(s));
    }

    if (force || s->profile_slot != ui.last.profile_slot) {
        fmt_link_digit(buf, sizeof(buf), s);
        set_text(ui.lbl_bt_digit, buf);
    }

    if (force || s->battery_left != ui.last.battery_left) {
        fmt_battery_text(buf, sizeof(buf), s->battery_left);
        set_text(ui.lbl_batt_l_text, buf);
        set_gauge(ui.gauge_l_body, ui.gauge_l_fill, ui.gauge_l_nub, s->battery_left);
    }

    if (force || s->battery_right != ui.last.battery_right) {
        fmt_battery_text(buf, sizeof(buf), s->battery_right);
        set_text(ui.lbl_batt_r_text, buf);
        set_gauge(ui.gauge_r_body, ui.gauge_r_fill, ui.gauge_r_nub, s->battery_right);
    }

    if (force || s->modifiers != ui.last.modifiers) {
        static const uint8_t mod_bits[MOD_COUNT] = {
            DISPSCAN_MOD_CTRL,
            DISPSCAN_MOD_SHIFT,
            DISPSCAN_MOD_ALT,
            DISPSCAN_MOD_GUI,
        };

        for (int i = 0; i < MOD_COUNT; i++) {
            set_hidden(ui.mod_underline[i], (s->modifiers & mod_bits[i]) == 0);
        }
    }

    if (force || s->freshness != ui.last.freshness) {
        /* Blank rather than a "LIVE" counterpart: the steady state is fresh, and
         * a label that is present 99% of the time trains the eye straight past
         * the 1% that matters. Same reasoning as CAPS below. */
        bool stale = (s->freshness != DISPSCAN_FRESH_LIVE);

        set_text(ui.lbl_stale, stale ? STALE_MARKER_TEXT : "");
        set_hidden(ui.lbl_stale, !stale);
    }

    if (force || s->caps_word != ui.last.caps_word) {
        /* Blank rather than a greyed placeholder: caps-word is rare and
         * transient, so permanent "caps" chrome would train the eye past it.
         * The modifier keycaps are permanent for the opposite reason -- they own
         * fixed columns, so their absence would move the layout. */
        set_text(ui.lbl_caps, s->caps_word ? "CAPS" : "");
        set_hidden(ui.lbl_caps, !s->caps_word);
    }

    /* NEITHER WPM NOR keyboard_id IS DRAWN. Both are still decoded and both
     * still cross the seam; they were removed from the composition on request.
     * The id's binding procedure moved to the serial log -- see the band D
     * comment, which is where that consequence is written down. */

    if (force || s->rssi != ui.last.rssi) {
        /* "-128dBm" is the worst case, 7 chars = 56 px at x=99..155. A value of
         * 0 means "no radio produced this" (the fake source), not 0 dBm. */
        snprintf(buf, sizeof(buf), "%ddBm", (int)s->rssi);
        set_text(ui.lbl_rssi, buf);
    }
}

static void render(const struct dispscan_status *s) {
    /* A state change repaints every band; a same-state update repaints only the
     * bands whose inputs moved. */
    bool state_changed = !ui.has_last || s->link != ui.last.link;

    switch (s->link) {
    case DISPSCAN_LINK_AWAKE:
        if (state_changed) {
            lv_obj_set_style_bg_color(ui.screen, COL_BG, LV_PART_MAIN);
            set_hidden(ui.lbl_nosig, true);
            set_hidden(ui.lbl_nosig_sub, true);
            set_awake_hidden(false);
            FAKE_MARKER_SET_DARK(false);
        }
        render_awake(s, state_changed);
        break;

    case DISPSCAN_LINK_DARK:
        if (state_changed) {
            /*
             * A DRAWN black frame -- see the COL_DARK note above. The objects
             * keep their contents underneath, so waking is a hide/show plus a
             * background colour change and never a rebuild; that is what makes
             * the wake repaint instant (plan doc, D6: "state retained in RAM so
             * wake redraw is instant").
             *
             * We deliberately do NOT stop ZMK's display tick here. The plan
             * doc's ordering rule (load the blank screen, let one frame flush,
             * THEN stop the timer) applies to the later power slice that
             * introduces tick stopping; leaving the tick running means there is
             * no window in which the black frame could fail to reach the panel.
             */
            lv_obj_set_style_bg_color(ui.screen, COL_DARK, LV_PART_MAIN);
            set_hidden(ui.lbl_nosig, true);
            set_hidden(ui.lbl_nosig_sub, true);
            set_awake_hidden(true);
            /* Not "almost blank" any more when the data is synthetic: the FAKE
             * marker stays. */
            FAKE_MARKER_SET_DARK(true);
        }
        break;

    case DISPSCAN_LINK_NO_SIGNAL:
    default:
        if (state_changed) {
            lv_obj_set_style_bg_color(ui.screen, COL_BG, LV_PART_MAIN);
            set_awake_hidden(true);
            set_hidden(ui.lbl_nosig, false);
            set_hidden(ui.lbl_nosig_sub, false);
            FAKE_MARKER_SET_DARK(false);
        }
        break;
    }

    ui.last = *s;
    ui.has_last = true;
}

/* -------------------------------------------------------------------------
 * Update entry point
 *
 * Same shape as ZMK's ZMK_DISPLAY_WIDGET_LISTENER
 * (zmk/app/include/zmk/display.h:31-60): a mutex-guarded state copy in the
 * producer's context, then k_work_submit_to_queue(zmk_display_work_q(), ...) so
 * that every LVGL call happens on the display thread. With `dispscan nice_view`
 * that queue is the DEDICATED one -- nice_view's Kconfig.defconfig sets
 * `choice ZMK_DISPLAY_WORK_QUEUE default ZMK_DISPLAY_WORK_QUEUE_DEDICATED` --
 * i.e. genuinely a different thread from the caller, so the mutex is doing real
 * work rather than documenting an intention.
 * ------------------------------------------------------------------------- */

/* K_MUTEX_DEFINE / K_WORK_DEFINE both emit file-scope symbols with EXTERNAL
 * linkage into iterable sections, so these names live in the whole-image
 * namespace. Prefixed accordingly -- a bare `pending_mutex` would be a link
 * error waiting for the first ZMK or Zephyr file to pick the same word. */
K_MUTEX_DEFINE(dispscan_pending_mutex);
static struct dispscan_status pending;

/* False until the first dispscan_status_update(). Distinguishes "nothing has
 * ever arrived" from "a zeroed status arrived", which the replay in
 * zmk_display_status_screen() must not confuse -- a zeroed struct is a valid
 * AWAKE frame, not an absence. Never cleared: `pending` is a state snapshot, so
 * once it is meaningful it stays meaningful. */
static bool pending_valid;

/*
 * Copy out the stored status under the mutex. PEEK, not take: it does not clear
 * `pending_valid`, because the value is the current state rather than a queued
 * event -- re-reading it is always correct and dropping it never is.
 */
static bool peek_pending(struct dispscan_status *out) {
    bool valid;

    k_mutex_lock(&dispscan_pending_mutex, K_FOREVER);
    valid = pending_valid;
    if (valid) {
        *out = pending;
    }
    k_mutex_unlock(&dispscan_pending_mutex);

    return valid;
}

static void update_work_cb(struct k_work *work) {
    struct dispscan_status snapshot;

    if (!peek_pending(&snapshot)) {
        return;
    }

    if (!ui.built) {
        /* Screen not constructed yet. Dropping THIS paint is safe because the
         * value is not dropped with it: `pending` and `pending_valid` both
         * survive, construction runs on this same work queue, and
         * zmk_display_status_screen() replays them as its last step. That replay
         * is what makes the header's pre-init guarantee true -- before it
         * existed, an update that arrived here early was simply lost. */
        return;
    }

    render(&snapshot);
}

K_WORK_DEFINE(dispscan_update_work, update_work_cb);

void dispscan_status_update(const struct dispscan_status *s) {
    if (s == NULL) {
        return;
    }

    k_mutex_lock(&dispscan_pending_mutex, K_FOREVER);
    pending = *s;
    pending_valid = true;
    k_mutex_unlock(&dispscan_pending_mutex);

    /* zmk_display_is_initialized() reports whether ZMK has started the display
     * work queue and entered initialize_display() (app/src/display/main.c:104).
     * Submitting before that would queue onto a thread that does not exist yet.
     *
     * Returning here does NOT lose the update: it is already stored above, and
     * zmk_display_status_screen() replays the stored value when it builds the
     * screen. That is the mechanism behind the header's "safe to call before the
     * display has initialised" guarantee. */
    if (!zmk_display_is_initialized()) {
        return;
    }

    k_work_submit_to_queue(zmk_display_work_q(), &dispscan_update_work);
}
