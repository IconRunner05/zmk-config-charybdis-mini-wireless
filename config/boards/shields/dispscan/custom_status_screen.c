/*
 * dispscan — landscape status screen for the nice!view (160x68, 1bpp).
 *
 * SPDX-License-Identifier: MIT
 *
 * SLICE SCOPE: the render path only. There is no BLE code here and none is
 * wanted yet — this file is driven entirely through dispscan_status_update(),
 * whose only caller today is dispscan_fake_source.c. Proving fonts, layout and
 * redraw against a fake source is build-order step 2 in
 * docs/remote-display-plan.md; the observer is step 3 and changes nothing here.
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
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <lvgl.h>

#include <zmk/display.h>
#include <zmk/display/status_screen.h>

#include "dispscan_status.h"

/* -------------------------------------------------------------------------
 * Geometry
 *
 * lv_font_unscii_8 is a fixed 8x8 bitmap face: every glyph descriptor in
 * lvgl/src/font/lv_font_unscii_8.c carries `.adv_w = 128` (LVGL stores advance
 * in 1/16 px, so 128/16 = 8 px) and the font header declares `.line_height = 9`.
 * With the default letter_space of 0 that is exactly 20 characters across the
 * 160 px panel and 7 lines down its 68 px -- so every width below can be
 * checked by counting characters, not by guessing.
 * ------------------------------------------------------------------------- */
#define PANEL_W 160
#define PANEL_H 68

/* Left/right text inset. 2 px keeps the panel edge from eating a glyph column
 * while still leaving 156 px = 19 usable characters per line. */
#define MARGIN 2

/*
 * BAND LAYOUT -- this bounds invalidation HEIGHT. It is not what keeps
 * ls0xx_write() happy; that is already guaranteed elsewhere.
 *
 * CORRECTION (this comment previously claimed the opposite). ls0xx_write() does
 * reject any `desc->width != LS0XX_PANEL_WIDTH`, but that rejection can never
 * fire here and the band layout is not why. The driver reports
 * `current_pixel_format = PIXEL_FORMAT_MONO01`, so Zephyr's LVGL glue
 * (zephyr/modules/lvgl/lvgl_display.c) registers `lvgl_rounder_cb_mono`, and
 * that callback unconditionally does `area->x1 = 0; area->x2 =
 * cap.x_resolution - 1`. EVERY flush is therefore already full-width before it
 * reaches the driver, whatever shape the invalidated object was. The driver
 * also advertises SCREEN_INFO_X_ALIGNMENT_WIDTH, which is what tells the glue
 * this is required.
 *
 * What the bands actually buy is the other axis: the mono rounder widens x but
 * leaves y alone, so an object's y extent is exactly the number of scanlines
 * that get pushed over SPI. Giving each independently-changing field its own y
 * band bounds a single-field update to ~9 rows instead of all 68. Never put a
 * tall changing object here: a 1 px change inside it would cost every row it
 * spans.
 *
 *   y=1   band A  layer name / number        | endpoint + BLE profile (right)
 *   y=15  band B  left-half battery
 *   y=26  band C  right-half battery
 *   y=40  band D  modifiers | STALE marker   | caps-word              (right)
 *   y=53  band E  WPM                        | keyboard id            (right)
 *
 * Bands A and D each hold two labels because those pairs tend to change
 * together (endpoint state, shift state) -- splitting them would buy no
 * invalidation and cost a band. Last band ends at 53+9 = 62 <= 68.
 */
#define BAND_A_Y 1
#define BAND_B_Y 15
#define BAND_C_Y 26
#define BAND_D_Y 40
#define BAND_E_Y 53

/* Battery gauge cell count. "L 100% [########]" = 17 chars = 136 px; with
 * MARGIN that is 138 of 160. The N/A rendering is the same width by
 * construction (see fmt_battery), so no value the source can produce clips. */
#define GAUGE_CELLS 8

/* -------------------------------------------------------------------------
 * Glyph coverage
 *
 * lv_font_unscii_8 defines glyphs for ASCII 0x20..0x7E and nothing else -- see
 * the `range_start`/`range_length` in lvgl/src/font/lv_font_unscii_8.c. Any
 * other byte renders as LVGL's missing-glyph box (or nothing), which both
 * breaks the character-counting width arithmetic above and hides the fact that
 * the input was malformed.
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
 * Object tree -- built ONCE in zmk_display_status_screen(), mutated thereafter.
 *
 * Rebuilding the tree per update would allocate out of LV_Z_MEM_POOL_SIZE on
 * every beacon and invalidate the whole screen each time, defeating the band
 * layout above.
 * ------------------------------------------------------------------------- */
struct dispscan_ui {
    bool built;

    lv_obj_t *screen;

    /* AWAKE composition. */
    lv_obj_t *lbl_layer;
    lv_obj_t *lbl_link;
    lv_obj_t *lbl_batt_l;
    lv_obj_t *lbl_batt_r;
    lv_obj_t *lbl_mods;
    lv_obj_t *lbl_stale;
    lv_obj_t *lbl_caps;
    lv_obj_t *lbl_wpm;
    lv_obj_t *lbl_kbid;

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
 * GEOMETRY. "FAKE" is 4 chars = 32 px. Right-aligned with a -50 px offset, so
 * its right edge is at 160-50 = 110 and it spans x=78..110 in band A. Band A's
 * two existing labels are the layer (worst case "L255 ABCD" = 9 chars = 72 px
 * from x=2, ending at 74) and the right-aligned link ("* BT4" = 5 chars = 40 px,
 * starting at 118). So the marker sits in dead space with a 4 px gap left and an
 * 8 px gap right, and nothing on the panel moves.
 *
 * Band A specifically because it is the only band the NO_SIGNAL text does not
 * reach: those two centred lines occupy roughly y=23..45, and band A is y=1..10.
 *
 * DELIBERATELY NOT IN collect_awake_objs(). It must stay visible in DARK and
 * NO_SIGNAL too -- those are precisely the states that would otherwise look
 * like a real keyboard idling or out of range.
 */
#define FAKE_MARKER_X (-50)
#define FAKE_MARKER_TEXT "FAKE"
#endif /* CONFIG_DISPSCAN_FAKE_SOURCE */

/* -------------------------------------------------------------------------
 * STALENESS MARKER — the second state axis, made visible.
 *
 * `link` says WHICH SCREEN to draw; `freshness` says whether the values on it
 * are current. They are independent (see the two-axes block in
 * dispscan_status.h), and before this label there was nowhere to put the
 * second one -- the panel could show a perfectly plausible AWAKE composition
 * whose numbers were 80 seconds old with no indication whatsoever.
 *
 * It must read as neither DARK nor NO_SIGNAL, and it does: DARK is an all-black
 * frame with every label hidden, NO_SIGNAL is the normal field carrying two
 * centred lines, and this is the normal composition plus five characters.
 *
 * GEOMETRY, re-derived rather than assumed (unscii_8 = 8 px advance, exactly):
 *   band D left  "MOD C.AG"  8 chars = 64 px at x=MARGIN(2)  -> x=2..66
 *   THIS         "STALE"     5 chars = 40 px at x=74         -> x=74..114
 *   band D right "CAPS"      4 chars = 40-ish, right-aligned at -MARGIN
 *                                                             -> x=118..158
 * Gaps: 8 px to the modifiers, 4 px to CAPS. Nothing else lives in band D and
 * no other label moves.
 *
 * Band D rather than band A because band A's spare space is already claimed by
 * the FAKE marker, and while the two are mutually exclusive by Kconfig today,
 * sharing a slot would make that exclusion load-bearing for LAYOUT as well as
 * behaviour. Band D is empty in the middle in every configuration.
 *
 * NEEDS ON-GLASS RE-VERIFICATION. Every other number in this file was checked
 * against a real panel; this one is derived from the same font metrics but has
 * not yet been photographed.
 * ------------------------------------------------------------------------- */
#define STALE_MARKER_X 74
#define STALE_MARKER_TEXT "STALE"

#define AWAKE_OBJ_COUNT 9

/* Everything that is visible only in AWAKE, so state switching is one loop.
 * Filled on each call rather than cached because the pointers are only valid
 * after construction and this is off the hot path (state transitions only). */
static void collect_awake_objs(lv_obj_t *objs[AWAKE_OBJ_COUNT]) {
    objs[0] = ui.lbl_layer;
    objs[1] = ui.lbl_link;
    objs[2] = ui.lbl_batt_l;
    objs[3] = ui.lbl_batt_r;
    objs[4] = ui.lbl_mods;
    objs[5] = ui.lbl_caps;
    objs[6] = ui.lbl_wpm;
    objs[7] = ui.lbl_kbid;
    /* In the AWAKE set on purpose: a staleness marker on a DARK or NO_SIGNAL
     * screen would be answering a question the user is not asking, and in
     * NO_SIGNAL it would be actively wrong -- there is nothing stale, there is
     * nothing at all. */
    objs[8] = ui.lbl_stale;
}

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

/*
 * lv_label_set_text() dereferences its object immediately, and EVERY label
 * pointer in `ui` is allowed to be NULL -- make_label() returns NULL when LVGL's
 * pool is exhausted rather than letting the caller fault. Routing every text
 * write through here is what turns "undersized LV_Z_MEM_POOL_SIZE" from a hard
 * fault inside the boot-time status-screen constructor (blank panel, no serial,
 * no diagnostic) into a partially-drawn screen plus a LOG_ERR.
 */
static void set_text(lv_obj_t *obj, const char *txt) {
    if (obj == NULL) {
        return;
    }
    lv_label_set_text(obj, txt);
}

#ifdef CONFIG_DISPSCAN_FAKE_SOURCE
/* The marker is never hidden, in any state, including DARK.
 *
 * It used to flip colour in DARK, back when COL_DARK was believed to paint a
 * bright frame. It does not -- see the colour block above; DARK's field is the
 * same panel-black as COL_BG -- so the content colour is COL_FG throughout and
 * the flip would have made the marker invisible, which is the precise failure
 * the marker exists to prevent.
 *
 * Deliberately visible even in DARK: a dark screen that is quietly showing
 * synthetic data is still a lie, and this is a dev-only build. `dark` is kept
 * in the signature so the call sites stay symmetric with the other state
 * transitions.
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

static void set_awake_hidden(bool hidden) {
    lv_obj_t *objs[AWAKE_OBJ_COUNT];

    collect_awake_objs(objs);
    for (int i = 0; i < AWAKE_OBJ_COUNT; i++) {
        set_hidden(objs[i], hidden);
    }
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
 * "L 100% [########]" / "R  87% [######..]" / "L   NA [--------]"
 *
 * Field widths are fixed (1 side char, space, 4-char value, space, bracketed
 * 8-cell gauge) so the two battery lines align vertically and the worst case is
 * a known 17 characters = 136 px.
 *
 * pct == 0 is N/A per the wire contract, NOT an empty battery -- rendered as
 * "NA" plus a dashed gauge, which cannot be mistaken for the 1% case ("  1%"
 * plus a one-cell gauge).
 *
 * pct > 100 IS NOT CLAMPED, and this is deliberate -- it used to be, and that
 * was wrong on both counts:
 *
 *   * The stated justification ("clamped rather than widening the field") was
 *     factually false. `snprintf(value, 5, "%3u%%", 255)` produces "255%" -- 4
 *     characters plus the NUL, which is exactly sizeof(value). The 255 case is
 *     the widest one and it already fits, so nothing was ever at risk of
 *     widening and no geometry depended on the clamp.
 *   * It contradicted fmt_link() below, which refuses to clamp an out-of-range
 *     profile precisely "because that would be a decoder bug worth seeing on the
 *     panel". Battery is the field where laundering is most dangerous: a
 *     one-byte offset error in the future decoder would clamp two garbage bytes
 *     into two healthy-looking batteries, which is the most reassuring possible
 *     rendering of a completely broken decode.
 *
 * So an out-of-range byte shows its real value AND a '?'-filled gauge. Same 17
 * characters, unmistakably not a battery reading.
 */
static void fmt_battery(char *out, size_t out_len, char side, uint8_t pct) {
    char gauge[GAUGE_CELLS + 1];
    char value[5];

    if (pct == 0) {
        memset(gauge, '-', GAUGE_CELLS);
        memcpy(value, "  NA", sizeof("  NA"));
    } else if (pct > 100) {
        /* Impossible per the wire contract; show it rather than hide it. */
        memset(gauge, '?', GAUGE_CELLS);
        snprintf(value, sizeof(value), "%3u%%", (unsigned int)pct);
    } else {
        /* Round to nearest cell, then floor at one so a live-but-nearly-flat
         * half never renders as the all-empty gauge that would read as N/A. */
        int cells = (pct * GAUGE_CELLS + 50) / 100;
        if (cells < 1) {
            cells = 1;
        }
        for (int i = 0; i < GAUGE_CELLS; i++) {
            gauge[i] = (i < cells) ? '#' : '.';
        }
        /* %3u%% keeps the value column exactly 4 wide across 1..100. */
        snprintf(value, sizeof(value), "%3u%%", (unsigned int)pct);
    }
    gauge[GAUGE_CELLS] = '\0';

    snprintf(out, out_len, "%c %s [%s]", side, value, gauge);
}

/*
 * "L2 NAV" -- name preferred, number always present so a blank name still
 * identifies the layer. Worst case is "L255 ABCD" = 9 chars = 72 px, ending at
 * x=74; the right-aligned endpoint label is 5 chars and starts at x=118, so
 * they cannot collide.
 */
static void fmt_layer(char *out, size_t out_len, const struct dispscan_status *s) {
    /*
     * SANITISE FIRST. The decoded contract guarantees NUL-termination and
     * nothing else about the CONTENT: zmk_keymap_layer_name() returns arbitrary
     * user text, and the 4-byte fixed wire field truncates without regard for
     * encoding -- a layer called "Nav->" written with a multi-byte arrow ships
     * as 4E 61 76 E2, a UTF-8 lead byte with its continuation bytes cut off.
     * unscii_8 has no glyph above 0x7E in any case, so an unsanitised name both
     * renders as garbage and breaks the 8 px-per-character width arithmetic this
     * whole layout rests on.
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
 * "U BT1" / "* BT0" / "- BT4" -- leading glyph is the active endpoint, trailing
 * field is the BLE profile slot the keyboard reports.
 *
 *   'U'  USB HID ready: the host is taking keystrokes over the cable.
 *   '*'  BLE connected AND bonded.
 *   '+'  BLE connected, not bonded (a Just Works session).
 *   '-'  nothing connected; the profile shown is merely the selected one.
 *
 * profile_slot arrives already masked & 0x07 (trap #1), so a value above 4 means
 * the keyboard reported a slot outside ZMK's profile count -- shown as '?'
 * rather than silently clamped, because that would be a decoder bug worth
 * seeing on the panel.
 */
static void fmt_link(char *out, size_t out_len, const struct dispscan_status *s) {
    char endpoint;

    if (s->usb_hid_ready) {
        endpoint = 'U';
    } else if (s->ble_connected) {
        endpoint = s->ble_bonded ? '*' : '+';
    } else {
        endpoint = '-';
    }

    if (s->profile_slot <= DISPSCAN_PROFILE_MAX) {
        snprintf(out, out_len, "%c BT%u", endpoint, (unsigned int)s->profile_slot);
    } else {
        snprintf(out, out_len, "%c BT?", endpoint);
    }
}

/*
 * "MOD C.AG" -- one fixed column per modifier class, letter when held and '.'
 * when not, so the string never changes width and the eye reads a column
 * position instead of parsing a list. Left and right are not distinguished
 * because the wire cannot express it (trap #4).
 */
static void fmt_mods(char *out, size_t out_len, uint8_t mods) {
    snprintf(out, out_len, "MOD %c%c%c%c", (mods & DISPSCAN_MOD_CTRL) ? 'C' : '.',
             (mods & DISPSCAN_MOD_SHIFT) ? 'S' : '.', (mods & DISPSCAN_MOD_ALT) ? 'A' : '.',
             (mods & DISPSCAN_MOD_GUI) ? 'G' : '.');
}

/* -------------------------------------------------------------------------
 * Construction
 * ------------------------------------------------------------------------- */

static lv_obj_t *make_label(lv_obj_t *parent, lv_align_t align, int32_t x, int32_t y) {
    lv_obj_t *lbl = lv_label_create(parent);

    /*
     * FAIL LOUDLY AND SURVIVABLY, never fault. lv_label_create() allocates out
     * of LV_Z_MEM_POOL_SIZE and returns NULL when that pool is exhausted; every
     * style call below would then dereference it. Faulting HERE is the worst
     * possible place for it: this function runs inside the boot-time status
     * screen constructor, long before USB CDC has enumerated, so the symptom
     * would be a blank panel with no serial output and no diagnostic whatsoever.
     *
     * Returning NULL instead costs one label and keeps the rest of the screen --
     * set_text() and set_hidden() both tolerate NULL, so the failure degrades to
     * "one field is missing" plus a log line that survives to the next boot with
     * a console attached.
     */
    if (lbl == NULL) {
        LOG_ERR("dispscan: lv_label_create failed at (%d,%d) -- LVGL pool exhausted "
                "(CONFIG_LV_Z_MEM_POOL_SIZE=%d). This field will not be drawn.",
                (int)x, (int)y, (int)CONFIG_LV_Z_MEM_POOL_SIZE);
        return NULL;
    }

    /*
     * unscii_8 is set per-object rather than relying on the theme: ZMK only
     * installs a theme under CONFIG_LV_USE_THEME_MONO, which this build does not
     * enable (app/src/display/Kconfig only `imply`s it for the BUILT_IN status
     * screen, and this shield selects the CUSTOM one). Without a theme an object
     * inherits no font, so this is mandatory rather than stylistic.
     */
    lv_obj_set_style_text_font(lbl, &lv_font_unscii_8, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, COL_FG, LV_PART_MAIN);
    lv_label_set_text(lbl, "");
    lv_obj_align(lbl, align, x, y);
    return lbl;
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

    ui.screen = lv_obj_create(NULL);

    /*
     * Same rule as make_label(): no unchecked dereference in the boot path. If
     * even the root object cannot be allocated there is no screen to build, so
     * return NULL -- app/src/display/main.c handles that explicitly
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

    ui.lbl_layer = make_label(ui.screen, LV_ALIGN_TOP_LEFT, MARGIN, BAND_A_Y);
    ui.lbl_link = make_label(ui.screen, LV_ALIGN_TOP_RIGHT, -MARGIN, BAND_A_Y);
    ui.lbl_batt_l = make_label(ui.screen, LV_ALIGN_TOP_LEFT, MARGIN, BAND_B_Y);
    ui.lbl_batt_r = make_label(ui.screen, LV_ALIGN_TOP_LEFT, MARGIN, BAND_C_Y);
    ui.lbl_mods = make_label(ui.screen, LV_ALIGN_TOP_LEFT, MARGIN, BAND_D_Y);
    ui.lbl_stale = make_label(ui.screen, LV_ALIGN_TOP_LEFT, STALE_MARKER_X, BAND_D_Y);
    ui.lbl_caps = make_label(ui.screen, LV_ALIGN_TOP_RIGHT, -MARGIN, BAND_D_Y);
    ui.lbl_wpm = make_label(ui.screen, LV_ALIGN_TOP_LEFT, MARGIN, BAND_E_Y);
    ui.lbl_kbid = make_label(ui.screen, LV_ALIGN_TOP_RIGHT, -MARGIN, BAND_E_Y);

    /*
     * NO_SIGNAL composition. Deliberately the OPPOSITE of DARK: DARK is an
     * all-black frame with nothing on it, NO_SIGNAL is the normal white frame
     * carrying two centred lines. The two can never be confused, which matters
     * because they mean very different things ("keyboard idle, data still good"
     * vs "keyboard gone, data meaningless").
     *
     * "-- NO SIGNAL --" is 15 chars = 120 px, centred with 20 px either side.
     */
    ui.lbl_nosig = make_label(ui.screen, LV_ALIGN_CENTER, 0, -6);
    set_text(ui.lbl_nosig, "-- NO SIGNAL --");
    ui.lbl_nosig_sub = make_label(ui.screen, LV_ALIGN_CENTER, 0, 6);
    set_text(ui.lbl_nosig_sub, "no beacon heard");

#ifdef CONFIG_DISPSCAN_FAKE_SOURCE
    /* See FAKE_MARKER_X above for the geometry derivation and for why this one
     * label is exempt from every hide/show path. */
    ui.lbl_fake = make_label(ui.screen, LV_ALIGN_TOP_RIGHT, FAKE_MARKER_X, BAND_A_Y);
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

    LOG_INF("dispscan: status screen built (%dx%d landscape, no rotation)", PANEL_W, PANEL_H);

    /*
     * REPLAY THE PRE-INIT UPDATE. dispscan_status.h promises that
     * dispscan_status_update() is safe to call before the display exists and
     * that the stored update is painted once the screen is built. Without this
     * block that promise was simply false: the update stored `pending` and
     * returned without submitting (the queue did not exist yet), update_work_cb
     * dropped anything arriving before `ui.built`, and nothing ever read
     * `pending` again -- so a producer that pushed exactly once before init was
     * silently orphaned and the panel sat on NO_SIGNAL forever.
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
 * paint and whenever the AWAKE labels were hidden and are coming back, since
 * their contents may predate the DARK/NO_SIGNAL excursion. */
static void render_awake(const struct dispscan_status *s, bool force) {
    char buf[24];

    if (force || s->active_layer != ui.last.active_layer ||
        strcmp(s->layer_name, ui.last.layer_name) != 0) {
        fmt_layer(buf, sizeof(buf), s);
        set_text(ui.lbl_layer, buf);
    }

    if (force || s->profile_slot != ui.last.profile_slot ||
        s->usb_hid_ready != ui.last.usb_hid_ready || s->ble_connected != ui.last.ble_connected ||
        s->ble_bonded != ui.last.ble_bonded) {
        fmt_link(buf, sizeof(buf), s);
        set_text(ui.lbl_link, buf);
    }

    if (force || s->battery_left != ui.last.battery_left) {
        fmt_battery(buf, sizeof(buf), 'L', s->battery_left);
        set_text(ui.lbl_batt_l, buf);
    }

    if (force || s->battery_right != ui.last.battery_right) {
        fmt_battery(buf, sizeof(buf), 'R', s->battery_right);
        set_text(ui.lbl_batt_r, buf);
    }

    if (force || s->modifiers != ui.last.modifiers) {
        fmt_mods(buf, sizeof(buf), s->modifiers);
        set_text(ui.lbl_mods, buf);
    }

    if (force || s->freshness != ui.last.freshness) {
        /* Blank rather than a "LIVE" counterpart: the steady state is fresh, and
         * a label that is present 99% of the time trains the eye straight past
         * the 1% that matters. Same reasoning as CAPS below. */
        set_text(ui.lbl_stale,
                 (s->freshness == DISPSCAN_FRESH_LIVE) ? "" : STALE_MARKER_TEXT);
    }

    if (force || s->caps_word != ui.last.caps_word) {
        /* Blank rather than a greyed placeholder: caps-word is rare and
         * transient, so permanent "caps" chrome would train the eye past it. */
        set_text(ui.lbl_caps, s->caps_word ? "CAPS" : "");
    }

    if (force || s->wpm != ui.last.wpm) {
        /* "WPM 255" = 7 chars = 56 px ending at x=58; the right half of this
         * band starts at 160-2-56 = 102, so the two never touch. */
        snprintf(buf, sizeof(buf), "WPM %u", (unsigned int)s->wpm);
        set_text(ui.lbl_wpm, buf);
    }

    if (force || s->keyboard_id != ui.last.keyboard_id) {
        /*
         * ALL 32 BITS. This is the D8 binding key, and the ratified setup
         * procedure is "read the hex digits off the screen, paste them into the
         * shield .conf". Rendering only the low 16 broke that silently: the user
         * pastes a half-width value, the strict-equality allowlist never
         * matches, and the panel says NO SIGNAL with nothing to debug.
         *
         * "ID C0FFEE12" is 11 chars = 88 px. Right-aligned at -MARGIN it spans
         * x = 160-2-88 = 70 .. 158. Band E's left label is "WPM 255" = 7 chars =
         * 56 px at x=2..58, so there is a 12 px gap and 2 px to the panel edge.
         */
        snprintf(buf, sizeof(buf), "ID %08X", (unsigned int)s->keyboard_id);
        set_text(ui.lbl_kbid, buf);
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
             * A DRAWN black frame -- see the COL_DARK note above. The labels keep
             * their text underneath, so waking is a hide/show plus a background
             * colour change and never a rebuild; that is what makes the wake
             * repaint instant (plan doc, D6: "state retained in RAM so wake
             * redraw is instant").
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
             * marker stays, inverted to white so it is legible on black. */
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
