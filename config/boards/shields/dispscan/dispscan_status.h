/*
 * dispscan — the DECODED status model.
 *
 * SPDX-License-Identifier: MIT
 *
 * THIS IS THE SEAM. Everything upstream of this header (today: a fake source;
 * later: the BLE observer + packet decoder) produces a `struct dispscan_status`;
 * everything downstream (custom_status_screen.c) consumes one. The renderer must
 * never see a raw advertisement, and this struct must never gain a wire offset,
 * a bit mask, or a byte-order concern. That is what makes the observer slice a
 * drop-in.
 *
 * The wire format is the 26-byte status advertisement documented in
 * docs/remote-display-plan.md, "The wire contract". This struct is its DECODED
 * form: masks applied, strings terminated, flag bits unpacked. The decoder traps
 * from that document (numbered below as "trap N") are all resolved HERE, once,
 * rather than being re-litigated at every use site.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Wire `version` (offset 4) is a single packed byte: `[7:4]` major, `[3:0]`
 * minor. It is carried through this seam VERBATIM -- unpacked only at the point
 * of use, so the decoded struct still stores exactly one byte and no negotiation
 * logic is implied.
 *
 * It is here because without it the scanner cannot tell a v1 payload from a v2
 * one and would mis-decode a future layout as the current one -- silently, since
 * every field would still parse. The plan doc's "the only contract between them
 * is the byte layout" is only checkable if the layout carries its own label.
 *
 * NOTHING IN THIS SLICE ACTS ON IT. No gate, no fallback, no rejection: the
 * renderer does not read it and must not start to. The first consumer will be
 * the decoder, which is where a version check belongs.
 */
#define DISPSCAN_VERSION_MAJOR(v) (((v) >> 4) & 0x0Fu)
#define DISPSCAN_VERSION_MINOR(v) ((v) & 0x0Fu)

/*
 * Wire field is `char layer_name[4]`, zero-PADDED but NOT NUL-TERMINATED
 * (trap #2 — the upstream header comment claims otherwise; the implementation's
 * own comment says "Receiver must use %.4s or memcpy+null, never raw %s").
 * The decoded form is a plain C string, so it needs one more byte. A decoder
 * memcpy()s 4 bytes in and writes buf[4] = '\0' unconditionally.
 */
#define DISPSCAN_LAYER_NAME_WIRE_LEN 4
#define DISPSCAN_LAYER_NAME_BUF_LEN (DISPSCAN_LAYER_NAME_WIRE_LEN + 1)

/*
 * Modifier classes, already collapsed from the wire's 8 bits.
 *
 * TRAP #4 — LEFT AND RIGHT ARE NOT MODELLED. The wire byte has LCTL/LSFT/LALT/
 * LGUI/RCTL/RSFT/RALT/RGUI at bits 0..7, but the WRITER sets both the L and the
 * R bit for each class, so the distinction carries zero information. Presenting
 * it would be inventing data. A decoder therefore ORs the two bits per class
 * into one flag here.
 */
#define DISPSCAN_MOD_CTRL (1u << 0)
#define DISPSCAN_MOD_SHIFT (1u << 1)
#define DISPSCAN_MOD_ALT (1u << 2)
#define DISPSCAN_MOD_GUI (1u << 3)

/*
 * Highest profile slot the renderer will print as a number rather than "BT?".
 *
 * Wire `profile_slot` is `[6]`=dev, `[5:3]`=patch, `[2:0]`=profile, so the
 * decoded value is always 0..7; how many of those are REAL is a property of
 * the KEYBOARD's CONFIG_ZMK_BLE_PROFILE_COUNT, which — like the central side —
 * is not on the wire.
 *
 * It used to be a bare `4`, i.e. ZMK's default profile count hardcoded into
 * the scanner (plan doc, review finding #5): a keyboard built with 5 profiles
 * renders "BT?" for slot 5, which looks like a decoder bug and is not one.
 * Now a Kconfig, so mirroring the keyboard is a config line rather than a
 * source edit. Still a mirror of a value the wire does not carry — the same
 * class of hidden coupling as the battery swap, and worth the same suspicion.
 */
#define DISPSCAN_PROFILE_MAX CONFIG_DISPSCAN_PROFILE_MAX

/*
 * ===================== TWO AXES, NOT ONE — READ THIS =====================
 *
 * The plan doc's render-slice review, deferred finding #2, is explicit that
 * this header USED TO CONFLATE TWO INDEPENDENT THINGS into one enum:
 *
 *   AXIS 1 — FRESHNESS. "How long since I heard a packet." A property of the
 *            RADIO LINK. LIVE / IDLE / LOST.
 *   AXIS 2 — KEYBOARD ACTIVITY. "Is a human typing on it." A property of the
 *            KEYBOARD, inferred from advertising CADENCE (the broadcaster
 *            switches between a ~1 s active interval and a ~30 s idle one).
 *
 * They are genuinely orthogonal, and the case that proves it is ordinary: a
 * keyboard sitting untouched for 60 s is still beaconing happily at its idle
 * cadence. On axis 1 that is IDLE-with-a-staleness-marker (we last heard it up
 * to 30 s ago); on axis 2 it is heading for DARK. One enum could express
 * either but not both, so the observer would have had to pick one to lie about.
 *
 * MODEL CHOSEN: keep both, and derive a third value for the renderer.
 *
 *   `freshness`  raw axis 1                     — drives the STALE marker
 *   `link`       the RESOLVED SCREEN to draw    — drives which composition
 *
 * `link` stays exactly as it was (AWAKE / DARK / NO_SIGNAL), so the renderer's
 * state machine is unchanged and the render slice's on-glass validation still
 * holds. It is now explicitly a DERIVED field, computed once in dispscan_link.c
 * as:
 *
 *   freshness == LOST            -> NO_SIGNAL   (nothing heard; data is junk)
 *   else keyboard inactive       -> DARK        (heard fine; nobody is typing)
 *   else                         -> AWAKE
 *
 * NO_SIGNAL beats DARK when both apply: "the keyboard is gone" is strictly more
 * important than "the keyboard is idle", and D8 is explicit that the two must
 * never be confused. The three screens stay visually distinct: DARK is an
 * all-black frame with nothing on it, NO_SIGNAL is the normal field carrying
 * "-- NO SIGNAL --", and AWAKE-but-stale is the normal composition plus a
 * "STALE" marker. Three meanings, three appearances.
 *
 * WHY NOT COLLAPSE THEM. A single 4-value enum (AWAKE/STALE/DARK/NO_SIGNAL)
 * was considered and rejected: it cannot represent "dark AND stale", which is
 * the steady state of a keyboard that has been idle for minutes, and the
 * renderer would then have to reconstruct the missing axis from nothing.
 */

/*
 * AXIS 1 — how long since a matching packet was heard. Thresholds are
 * Kconfig-tunable; defaults are D8's ("Three liveness states").
 */
enum dispscan_freshness {
    /** Heard within CONFIG_DISPSCAN_LIVE_MS (default 5 s). */
    DISPSCAN_FRESH_LIVE = 0,
    /** Older than that but within CONFIG_DISPSCAN_LOST_MS (default 90 s).
     *  Values are still believed correct and are still SHOWN -- never zero out
     *  last-known values, a battery reading of 0% is worse than a stale one --
     *  but the panel carries a staleness marker. This window must span the
     *  keyboard's ~30 s idle cadence or a board merely sitting there reads as
     *  gone. */
    DISPSCAN_FRESH_IDLE,
    /** Nothing for CONFIG_DISPSCAN_LOST_MS. The data fields are meaningless.
     *  Also the boot state, before anything has ever been heard. */
    DISPSCAN_FRESH_LOST,
};

/*
 * THE RESOLVED SCREEN — D6, as amended by D9 (see docs/remote-display-plan.md,
 * "What survives of D6"). Drives ONLY what is drawn; since the display is
 * USB-powered these no longer drive scan parameters, which are constant.
 *
 * DERIVED from `freshness` plus inferred keyboard activity — see the block
 * above. A producer must not invent it independently of `freshness`.
 *
 * The renderer must handle all three from day one. The fake source cycles
 * through all three precisely so that DARK and NO_SIGNAL are proven to render
 * before there is any radio that could produce them.
 */
enum dispscan_link_state {
    /** A fresh beacon arrived recently and the keyboard is being used. */
    DISPSCAN_LINK_AWAKE = 0,
    /** Keyboard is idle. Data in this struct is still believed correct, but is
     *  deliberately not shown — never leave stale keyboard state on screen. */
    DISPSCAN_LINK_DARK,
    /** Nothing heard at all. The data fields are meaningless. */
    DISPSCAN_LINK_NO_SIGNAL,
};

/**
 * Decoded keyboard status. Plain old data — copyable by assignment, which is
 * what makes the hand-off to the display work queue a single struct copy under
 * a mutex rather than a pointer with a lifetime problem.
 */
struct dispscan_status {
    /** Which of the three D6 screens to draw. DERIVED — see the two-axes block
     *  above. Valid in every state. */
    enum dispscan_link_state link;

    /** Axis 1, raw. Drives the staleness marker, which must stay visually
     *  distinct from BOTH the DARK frame and the NO_SIGNAL screen. */
    enum dispscan_freshness freshness;

    /*
     * RSSI of the packet this status came from, in dBm (Zephyr's
     * bt_le_scan_recv_info.rssi, zephyr/include/zephyr/bluetooth/bluetooth.h).
     *
     * Carried because the plan doc's review finding #4 records its absence as
     * BLOCKING two ratified behaviours, not as a nice-to-have: D8's discovery
     * mode is defined as "render the keyboard_id of the STRONGEST keyboard it
     * hears", and the two-keyboard rule needs a strength comparison to break a
     * tie. Both live in dispscan_observer.c, which is why this field is set
     * there and not by the decoder -- signal strength is a property of the
     * radio, not of the payload.
     *
     * 0 is not a legal RSSI in practice (it would be 0 dBm at the antenna) and
     * is what a producer with no radio leaves here; the renderer does not
     * currently draw it, so no geometry depends on it.
     */
    int8_t rssi;

    /*
     * Raw wire `version` byte (offset 4), unpacked with DISPSCAN_VERSION_MAJOR /
     * DISPSCAN_VERSION_MINOR above.
     *
     * THE DECODER MUST POPULATE THIS. It is the one field a producer cannot
     * derive from anything else, and a zero here is indistinguishable from a
     * genuine v0.0 payload -- so a decoder that forgets it has silently
     * disabled every future version check built on top. Meaningless (and
     * conventionally 0) in DISPSCAN_LINK_NO_SIGNAL, where nothing was heard.
     */
    uint8_t version;

    /*
     * TRAP #3 — POSITIONAL, NOT ROLE-BASED. The wire carries `battery_level`
     * (offset 5) and `peripheral_battery[0]` (offset 12), and the central SWAPS
     * which of its two readings goes where based on
     * CONFIG_ZMK_STATUS_ADV_CENTRAL_SIDE (default "RIGHT", which matches our
     * keyboard). Decoding that swap is the decoder's job; by the time a value
     * reaches here it means "the left half" / "the right half" and nothing else.
     *
     * 0 means N/A per the wire contract (no reading available), NOT "flat".
     * The renderer must show something unmistakably different from 1%.
     */
    uint8_t battery_left;
    uint8_t battery_right;

    /*
     * Wire `active_layer` is written UNCLAMPED despite the upstream header's
     * "0-15" comment, so the full 0..255 range is representable and the
     * renderer must not assume a single digit.
     */
    uint8_t active_layer;

    /** NUL-terminated. Empty string when the wire bytes were all zero — the
     *  renderer then falls back to showing `active_layer` alone. */
    char layer_name[DISPSCAN_LAYER_NAME_BUF_LEN];

    /** Already masked `& 0x07` (trap #1). Reading the raw byte yields
     *  0x10 + profile on current keyboard firmware. */
    uint8_t profile_slot;

    /** OR of DISPSCAN_MOD_*. See trap #4 above. */
    uint8_t modifiers;

    /** Wire offset 24. Zero when the keyboard has CONFIG_ZMK_WPM=n, which is
     *  indistinguishable from "not typing" — do not infer activity from it. */
    uint8_t wpm;

    /*
     * Wire offset 19, hash of hwinfo_get_device_id(). Sent in LITTLE-ENDIAN
     * HOST order, so a decoder must use sys_get_le32() rather than a memcpy of
     * the struct field (see plan doc, "Byte-order trap"). This is the D8
     * binding key: it is the only permanent, non-rotating identifier in the
     * payload, and passive scanning does not deliver the keyboard's name.
     */
    uint32_t keyboard_id;

    /** status_flags 0x01. */
    bool caps_word;
    /** status_flags 0x04. */
    bool usb_connected;
    /** status_flags 0x08. */
    bool usb_hid_ready;
    /** status_flags 0x10. */
    bool ble_connected;
    /** status_flags 0x20. */
    bool ble_bonded;

    /*
     * DELIBERATELY ABSENT — status_flags 0x02, CHARGING.
     * TRAP #5: the bit is defined in the upstream header and NO CODE EVER SETS
     * IT. It is always 0 on the wire, so a `bool charging` field here would be a
     * permanently-false input that quietly acquires a widget nobody can trigger.
     * Add it only together with a keyboard-side writer.
     *
     * DELIBERATELY ABSENT — `connection_count` (wire offset 8). Hardcoded to 1,
     * +1 if USB HID is ready, i.e. it is a strictly worse encoding of
     * `usb_hid_ready`, which is already above.
     */
};

/**
 * Hand a freshly decoded status to the renderer.
 *
 * Thread-safety contract: this copies @p s under a mutex and then bounces the
 * actual LVGL work onto ZMK's display work queue, mirroring
 * ZMK_DISPLAY_WIDGET_LISTENER (zmk/app/include/zmk/display.h:31-60). Callers
 * therefore need not be on the display thread and must NOT touch LVGL
 * themselves.
 *
 * Because it takes a mutex, it MUST NOT be called from an ISR (a k_timer expiry
 * function is an ISR context). Bounce through a k_work first — see
 * dispscan_fake_source.c for the pattern.
 *
 * Safe to call before the display has initialised: the update is STORED and
 * REPLAYED -- zmk_display_status_screen() paints the stored value as part of
 * building the screen, so a producer that pushes exactly once before init still
 * gets that frame on the panel. This matters for the future BLE observer, which
 * may suppress duplicate payloads and therefore push only one update for a
 * keyboard whose state is not changing.
 *
 * Only the LATEST pre-init update survives; earlier ones are overwritten, which
 * is correct for a state snapshot (as opposed to an event stream).
 *
 * @param s decoded status; copied, not retained.
 */
void dispscan_status_update(const struct dispscan_status *s);
