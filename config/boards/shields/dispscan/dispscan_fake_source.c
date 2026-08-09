/*
 * dispscan — FAKE status source. DEVELOPMENT SCAFFOLD, NOT PRODUCT CODE.
 *
 * SPDX-License-Identifier: MIT
 *
 * ================== DELETE ME WHEN THE BLE OBSERVER LANDS ==================
 *
 * There is no radio in this slice. This file stands in for the BLE observer so
 * that fonts, layout, band invalidation and the three D6 display states can be
 * proven on real hardware before any scanning code exists — build-order step 2
 * in docs/remote-display-plan.md.
 *
 * Removal is one line in CMakeLists.txt plus one symbol in Kconfig.defconfig:
 * with CONFIG_DISPSCAN_FAKE_SOURCE=n nothing here links, the build still
 * compiles, and the renderer simply never receives an update (it boots into
 * NO_SIGNAL and stays there, which is the honest thing for a scanner that is
 * not scanning).
 *
 * IT SAYS SO ON THE GLASS. custom_status_screen.c draws a "FAKE" marker in
 * band A under #ifdef CONFIG_DISPSCAN_FAKE_SOURCE, in every display state
 * including DARK. Without it this file produces a completely convincing live
 * keyboard and the only warning is the LOG_WRN below, on a console that may not
 * be attached — so a photograph of the device, or the device itself a week
 * later, would be indistinguishable from working hardware.
 *
 * It deliberately produces ONLY a `struct dispscan_status` — the decoded form.
 * If a future edit finds itself wanting a wire offset or a bit mask in here,
 * that is the signal that the fake source has outgrown its job and the real
 * decoder should be written instead.
 */

#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include "dispscan_status.h"

/*
 * Cadence. Slow enough that a human can read each frame and confirm nothing
 * clips; fast enough that the full 40-step cycle completes in ~60 s, so a
 * bring-up session sees every state without waiting.
 */
#define TICK_MS 1500

/*
 * Cycle shape, in ticks:
 *   0..29   AWAKE, values sweeping
 *   30      AWAKE, WORST-CASE NUMERIC frame   (see below)
 *   31      AWAKE, WORST-CASE TEXT frame      (see below)
 *   32..35  DARK        (~6 s of black panel)
 *   36..39  NO_SIGNAL   (~6 s)
 * then wrap. 32 AWAKE steps is a multiple of every sweep period below (2, 4, 5,
 * 6, 16), so the visible sequence is genuinely periodic rather than drifting;
 * the last two steps are then overwritten wholesale, which does not disturb
 * that periodicity because the period is the whole 40-tick cycle.
 */
#define CYCLE_LEN 40
#define AWAKE_STEPS 32
#define DARK_STEPS 4

/*
 * WORST-CASE FRAMES -- the reason this file exists.
 *
 * The sweeps below are all safely in range: `a % 4` can never exceed 3, `(a %
 * 6) * 20` can never exceed 100, `a % 5` is always a legal profile, `(a * 17) %
 * 120` never reaches 3-digit-plus territory. So the layout's RISKIEST branches
 * -- the ones this whole slice exists to de-risk on glass -- were precisely the
 * ones never emitted: "L255 ABCD" (the widest layer string), the battery
 * out-of-range rendering, fmt_link()'s "BT?" fallback, "WPM 255", and the
 * non-ASCII layer-name substitution.
 *
 * Two ticks per cycle are therefore given over to those extremes. They are
 * deliberately at the END of the AWAKE run, immediately before the DARK
 * transition, so a human watching the panel sees them at a predictable moment
 * rather than having to catch them mid-sweep.
 */
#define WORST_NUMERIC_STEP (AWAKE_STEPS - 2)
#define WORST_TEXT_STEP (AWAKE_STEPS - 1)

/*
 * Layer names EXACTLY as they arrive on the wire: 4 bytes, zero-padded, NOT
 * NUL-terminated (trap #2). "BASE" is the worst case — it fills all four bytes,
 * so a decoder that trusts the buffer to be a C string reads past the end. The
 * fake source reproduces that hazard on purpose so the copy-and-terminate path
 * below is exercised rather than assumed.
 */
static const char wire_layer_names[4][DISPSCAN_LAYER_NAME_WIRE_LEN] = {
    {'B', 'A', 'S', 'E'},
    {'N', 'A', 'V', '\0'},
    {'N', 'U', 'M', '\0'},
    {'F', 'N', '\0', '\0'},
};

/* Widest legal name: 4 printable bytes, no padding, so "L255 ABCD" is the
 * 9-character worst case fmt_layer()'s geometry comment is derived from. */
static const char wire_layer_name_widest[DISPSCAN_LAYER_NAME_WIRE_LEN] = {'A', 'B', 'C', 'D'};

/*
 * A layer name the panel CANNOT render. 0xE2 is the UTF-8 lead byte of a
 * 3-byte codepoint -- exactly what a 4-byte fixed wire field does to a layer
 * called "Nav" + an arrow: the lead byte survives, its two continuation bytes
 * are truncated away. 0x01 stands in for an outright control byte.
 *
 * unscii_8 covers 0x20..0x7E only, so an unsanitised renderer would draw
 * missing-glyph boxes (or nothing) here and the width arithmetic would be
 * wrong. The panel should show "L200 Na??".
 */
static const char wire_layer_name_nonascii[DISPSCAN_LAYER_NAME_WIRE_LEN] = {'N', 'a', (char)0xE2,
                                                                           (char)0x01};

/*
 * Wire `version` byte (offset 4): [7:4] major, [3:0] minor. 0x10 = v1.0, the
 * format documented in the plan doc's wire table. Fixed, because a fake source
 * that varied it would be inventing a version-negotiation story this slice
 * deliberately does not have.
 */
#define FAKE_WIRE_VERSION 0x10

static uint32_t step;

/*
 * Fill @p s for tick @p n. Kept pure (no LVGL, no globals beyond `step`) so the
 * value sequence can be reasoned about by reading this one function.
 */
static void fake_fill(struct dispscan_status *s, uint32_t n) {
    uint32_t phase = n % CYCLE_LEN;
    uint32_t a = phase % AWAKE_STEPS; /* position within the AWAKE sweep */

    memset(s, 0, sizeof(*s));

    if (phase < AWAKE_STEPS) {
        s->link = DISPSCAN_LINK_AWAKE;
    } else if (phase < AWAKE_STEPS + DARK_STEPS) {
        s->link = DISPSCAN_LINK_DARK;
    } else {
        s->link = DISPSCAN_LINK_NO_SIGNAL;
    }

    /*
     * THE SECOND STATE AXIS. `freshness` is independent of `link` (see the
     * two-axes block in dispscan_status.h), so it gets its own sweep rather
     * than tracking the state above -- if it followed `link` the fake source
     * would be demonstrating a coupling the real system does not have.
     *
     * Every fourth AWAKE tick is stale, which is what puts the "STALE" marker
     * on the glass for a human to check. That marker's geometry is the one
     * number in custom_status_screen.c that has NOT been photographed on a
     * real panel, and this is the only way to photograph it before a
     * broadcaster exists.
     */
    s->freshness = ((a % 4) == 3) ? DISPSCAN_FRESH_IDLE : DISPSCAN_FRESH_LIVE;

    /* Plausible desk-range RSSI, swept so the field is visibly populated in a
     * log even though nothing draws it. -40 dBm to -95 dBm. */
    s->rssi = (int8_t)(-40 - (int)(a % 56));

    /*
     * Batteries: 0,20,40,60,80,100 and its mirror. Both sequences INCLUDE 0,
     * which is the wire contract's "N/A" and the case most likely to be
     * rendered wrong (as a flat battery). Both also hit 100, the 3-digit
     * worst-case width.
     */
    s->battery_left = (uint8_t)((a % 6) * 20);
    s->battery_right = (uint8_t)(100 - (a % 6) * 20);

    /* Layers 0..3 with the 4-byte wire names above. This is the decode the real
     * observer will do: memcpy exactly 4 bytes, then terminate unconditionally.
     * Never strncpy from a non-terminated source and never printf it with %s. */
    s->active_layer = (uint8_t)(a % 4);
    memcpy(s->layer_name, wire_layer_names[a % 4], DISPSCAN_LAYER_NAME_WIRE_LEN);
    s->layer_name[DISPSCAN_LAYER_NAME_WIRE_LEN] = '\0';

    /* Profiles 0..4 — ZMK's full profile count, already masked & 0x07 by a
     * real decoder (trap #1). */
    s->profile_slot = (uint8_t)(a % 5);

    /* All 16 combinations of the four modifier classes over 16 ticks, so every
     * column of "MOD C.AG" is proven to light independently. */
    s->modifiers = (uint8_t)(a % 16);

    /* Toggles every other tick — slow enough to see, fast enough to catch. */
    s->caps_word = ((a / 2) % 2) == 1;

    /* 0..119, hitting 0 (the "WPM off on the keyboard" case) and 3-digit
     * values. Coprime-ish stride so consecutive ticks differ visibly. */
    s->wpm = (uint8_t)((a * 17) % 120);

    /* Endpoint mix: cycles USB-HID, BLE-bonded, BLE-unbonded and nothing, which
     * is exactly the four glyphs fmt_link() can emit. */
    s->usb_connected = true;
    s->usb_hid_ready = (a % 4) == 0;
    s->ble_connected = (a % 4) == 1 || (a % 4) == 2;
    s->ble_bonded = (a % 4) == 1;

    /* Fixed, because the real one is a fixed hash of the keyboard's hardware ID
     * and D8 binds on it. A changing value here would hide a rendering bug in
     * the one field that must never change. */
    s->keyboard_id = 0xC0FFEE12u;

    /* Carried verbatim through the seam; nothing reads it yet (see
     * dispscan_status.h). Set anyway, because a fake source that leaves it zero
     * would model a decoder that forgot to populate it. */
    s->version = FAKE_WIRE_VERSION;

    /*
     * WORST-CASE OVERRIDES. Applied last so they win outright over the sweeps.
     * Only reachable while phase < AWAKE_STEPS, so DARK and NO_SIGNAL are
     * untouched.
     */
    if (phase == WORST_NUMERIC_STEP) {
        /* Every numeric field at or past its documented limit at once.
         *   layer 255 + a 4-char name -> "L255 ABCD", the 9-char worst case
         *   battery 255 / 101         -> the out-of-range rendering, both sides
         *   profile 7                 -> above DISPSCAN_PROFILE_MAX, so "BT?"
         *   wpm 255                   -> "WPM 255", the 7-char worst case
         *   all modifiers + caps      -> every column of "MOD CSAG" lit, "CAPS"
         * If any of these clips or overlaps, it is visible on this one frame. */
        s->active_layer = 255;
        memcpy(s->layer_name, wire_layer_name_widest, DISPSCAN_LAYER_NAME_WIRE_LEN);
        s->layer_name[DISPSCAN_LAYER_NAME_WIRE_LEN] = '\0';
        s->battery_left = 255;
        s->battery_right = 101; /* one past the limit, the subtle case */
        s->profile_slot = 7;    /* still <= 0x07, i.e. a legal decode of an illegal slot */
        s->wpm = 255;
        s->modifiers = DISPSCAN_MOD_CTRL | DISPSCAN_MOD_SHIFT | DISPSCAN_MOD_ALT |
                       DISPSCAN_MOD_GUI;
        s->caps_word = true;
        /* Nothing connected, so fmt_link() emits '-' alongside the "BT?". */
        s->usb_hid_ready = false;
        s->ble_connected = false;
        s->ble_bonded = false;
    } else if (phase == WORST_TEXT_STEP) {
        /* The text hazard: a truncated multi-byte codepoint plus a control byte
         * in a layer name. Expected on the panel: "L200 Na??". */
        s->active_layer = 200;
        memcpy(s->layer_name, wire_layer_name_nonascii, DISPSCAN_LAYER_NAME_WIRE_LEN);
        s->layer_name[DISPSCAN_LAYER_NAME_WIRE_LEN] = '\0';
    }
}

/*
 * k_timer expiry runs in ISR context, where dispscan_status_update() must not be
 * called (it takes a mutex). Bounce onto the system work queue first — the same
 * split ZMK uses everywhere it feeds the display from an event.
 */
static void fake_work_cb(struct k_work *work) {
    struct dispscan_status s;

    fake_fill(&s, step);
    step++;

    dispscan_status_update(&s);
}

/* K_WORK_DEFINE / K_TIMER_DEFINE emit externally-linked file-scope symbols, so
 * these names are image-global -- prefix them. */
K_WORK_DEFINE(dispscan_fake_work, fake_work_cb);

static void fake_timer_cb(struct k_timer *timer) { k_work_submit(&dispscan_fake_work); }

K_TIMER_DEFINE(dispscan_fake_timer, fake_timer_cb, NULL);

static int dispscan_fake_source_init(void) {
    LOG_WRN("dispscan: FAKE status source active (CONFIG_DISPSCAN_FAKE_SOURCE=y) "
            "- %u ms tick, %u-step cycle, worst-case frames at steps %u/%u. "
            "No BLE data is being received; the panel shows a FAKE marker.",
            (unsigned int)TICK_MS, (unsigned int)CYCLE_LEN, (unsigned int)WORST_NUMERIC_STEP,
            (unsigned int)WORST_TEXT_STEP);

    /*
     * First expiry is one full period out, not immediate: ZMK submits
     * initialize_display() to the display work queue from its own SYS_INIT, and
     * letting the screen exist before the first update arrives keeps the boot
     * sequence readable in the log. Correctness does not depend on it —
     * dispscan_status_update() is safe before the screen is built.
     */
    k_timer_start(&dispscan_fake_timer, K_MSEC(TICK_MS), K_MSEC(TICK_MS));
    return 0;
}

SYS_INIT(dispscan_fake_source_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
