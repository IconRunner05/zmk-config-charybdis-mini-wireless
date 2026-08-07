/*
 * dispscan — wire decoder implementation.
 *
 * SPDX-License-Identifier: MIT
 *
 * See dispscan_packet.h for the contract and for why this file has no radio,
 * no LVGL and no static state.
 */

#include <string.h>

#include <zephyr/sys/byteorder.h>

#include "dispscan_packet.h"

/*
 * =========================== THE L/R BATTERY SWAP ===========================
 *
 * TRAP #3, and the one hidden coupling in this whole design. Read this before
 * touching the two assignments at the bottom of the function.
 *
 * The wire has two battery bytes:
 *   offset 5  `battery_level`        — the ADVERTISING device's own reading
 *   offset 12 `peripheral_battery[0]` — the OTHER half's reading
 * (Only the central advertises; peripheral-role devices do not advertise at
 * all, so "advertising device" and "central" are the same thing here.)
 *
 * Those are ROLE positions. The screen shows PHYSICAL positions, left and
 * right. Converting one to the other needs one fact -- WHICH PHYSICAL HALF IS
 * THE CENTRAL -- and THAT FACT IS NOT ON THE WIRE. There is no field for it.
 * It lives in the keyboard's own CONFIG_ZMK_STATUS_ADV_CENTRAL_SIDE, on the
 * other side of the radio link, in a different firmware image, on a different
 * ZMK version.
 *
 * So the scanner has to mirror it, and CONFIG_DISPSCAN_CENTRAL_SIDE_RIGHT is
 * that mirror. It defaults to RIGHT because our Charybdis central is the right
 * half (docs/remote-display-plan.md, D7: "the central is the right half").
 *
 * >>> FLIPPING THE KEYBOARD'S CENTRAL TO THE LEFT HALF WITHOUT FLIPPING THIS
 * >>> SYMBOL SILENTLY SWAPS THE TWO BATTERIES ON SCREEN. Both readings stay
 * >>> plausible, both keep moving, nothing logs, nothing renders as an error.
 * >>> It is invisible until someone drains one half and watches the wrong bar
 * >>> fall.
 *
 * THIS FALSIFIES D3's HEADLINE CLAIM. The plan doc says "the only contract
 * between them is the advertisement payload byte layout" (D3). It is not: the
 * contract is the byte layout PLUS the value of a Kconfig symbol that never
 * appears in those bytes. Recorded here rather than smoothed over, because a
 * coupling nobody has written down is exactly the kind that survives a
 * refactor.
 *
 * ---- UNVERIFIED, AND IT MATTERS ----
 * The upstream broadcaster (t-ogura/prospector-zmk-module) is NOT vendored in
 * this workspace, so the direction of its own use of
 * CONFIG_ZMK_STATUS_ADV_CENTRAL_SIDE could not be read from source here. Two
 * readings of the plan doc are possible:
 *   (a) the keyboard writes ROLE order and the scanner does the L/R mapping
 *       — what this file implements, and what the plan doc's review finding #3
 *       asserts ("the scanner must hardcode a keyboard-side Kconfig value");
 *   (b) the keyboard already pre-swaps into L/R order, in which case doing it
 *       again here DOUBLE-SWAPS and the correct scanner setting is the
 *       opposite of the keyboard's.
 * Either way the symbol below is the single knob that fixes it, and the fix is
 * a rebuild. CONFIRM ON GLASS against a real broadcaster before trusting the
 * two bars: unplug one half and check the right bar is the one that goes N/A.
 */

enum dispscan_decode_result dispscan_packet_decode(const uint8_t *data, size_t len,
                                                   struct dispscan_status *out) {
    uint8_t flags;
    uint8_t mods;
    uint8_t central_batt;
    uint8_t other_batt;

    if (data == NULL || out == NULL) {
        return DISPSCAN_DECODE_TOO_SHORT;
    }

    /*
     * LENGTH BEFORE MAGIC, always. The magic compare reads bytes 0..3, so a
     * 2-byte manufacturer payload from some random beacon would read off the
     * end if the order were reversed. `>=` not `==`: trap #6's rule is
     * "manufacturer data >= 26 bytes", and a future minor version may append
     * fields after offset 25 that this decoder simply ignores.
     */
    if (len < DISPSCAN_WIRE_LEN) {
        return DISPSCAN_DECODE_TOO_SHORT;
    }

    /* TRAP #6 — the entire membership test. Anything else on air that happens
     * to carry manufacturer data reaches here and is rejected by these four
     * bytes; there is no other filter, by design. */
    if (data[DISPSCAN_OFF_MANUFACTURER_ID + 0] != DISPSCAN_MAGIC_0 ||
        data[DISPSCAN_OFF_MANUFACTURER_ID + 1] != DISPSCAN_MAGIC_1 ||
        data[DISPSCAN_OFF_SERVICE_UUID + 0] != DISPSCAN_MAGIC_2 ||
        data[DISPSCAN_OFF_SERVICE_UUID + 1] != DISPSCAN_MAGIC_3) {
        return DISPSCAN_DECODE_BAD_MAGIC;
    }

    /*
     * VERSION GATE. Checked BEFORE anything is written to `out`, so a rejected
     * payload leaves the caller's struct untouched and there is no
     * half-decoded state to render by accident.
     *
     * Only the MAJOR nibble gates. A minor bump means "same offsets, extra
     * bytes or extra flag bits", both of which this decoder tolerates: it
     * reads fixed offsets and masks the flag bits it knows.
     */
    if (DISPSCAN_VERSION_MAJOR(data[DISPSCAN_OFF_VERSION]) != DISPSCAN_WIRE_VERSION_MAJOR) {
        return DISPSCAN_DECODE_BAD_VERSION;
    }

    memset(out, 0, sizeof(*out));

    /* Carried verbatim; dispscan_status.h keeps it packed and unpacks at use.
     * Populating it is mandatory -- a zero here is indistinguishable from a
     * genuine v0.0 payload and would disable every future version check. */
    out->version = data[DISPSCAN_OFF_VERSION];

    /* See the long note above. `central_batt` / `other_batt` are role-named on
     * purpose: they are the only honest names for these two bytes, and the
     * one line that turns them into left/right is isolated below. */
    central_batt = data[DISPSCAN_OFF_BATTERY_LEVEL];
    other_batt = data[DISPSCAN_OFF_PERIPHERAL_BATTERY + 0];

#ifdef CONFIG_DISPSCAN_CENTRAL_SIDE_RIGHT
    out->battery_right = central_batt;
    out->battery_left = other_batt;
#else
    out->battery_left = central_batt;
    out->battery_right = other_batt;
#endif

    /* Written UNCLAMPED by the broadcaster despite its header's "0-15"
     * comment, so the full 0..255 range is legal on the wire and no clamp
     * happens here either -- the renderer is sized for "L255". */
    out->active_layer = data[DISPSCAN_OFF_ACTIVE_LAYER];

    /*
     * TRAP #1 — MASK & 0x07. The byte is [6]=dev, [5:3]=patch, [2:0]=profile.
     * Current keyboard firmware sets patch=2, so the raw byte reads 0x10 +
     * profile and an unmasked decode renders "BT16" for profile 0. This single
     * `& 0x07` is the entire fix and it must not migrate to the renderer:
     * dispscan_status.h promises the field arrives already masked.
     */
    out->profile_slot = data[DISPSCAN_OFF_PROFILE_SLOT] & 0x07u;

    /*
     * TRAP #2 — layer_name IS NOT NUL-TERMINATED. It is a fixed 4-byte,
     * zero-PADDED field; "BASE" fills all four bytes and leaves no room for a
     * terminator. memcpy exactly 4 and terminate unconditionally into the
     * 5-byte buffer dispscan_status.h sizes for exactly this.
     *
     * The renderer additionally substitutes non-ASCII bytes, because a 4-byte
     * truncation of a multi-byte codepoint is well-formed here and unrenderable
     * there. That is a font concern, not a wire concern, so it stays there.
     */
    memcpy(out->layer_name, &data[DISPSCAN_OFF_LAYER_NAME], DISPSCAN_LAYER_NAME_WIRE_LEN);
    out->layer_name[DISPSCAN_LAYER_NAME_WIRE_LEN] = '\0';

    /*
     * BYTE ORDER — offset 19, 4 bytes, LITTLE-ENDIAN, and this is the D8
     * binding key so getting it backwards is a silent permanent NO SIGNAL.
     *
     * The broadcaster does `memcpy(keyboard_id, &id_hash, 4)` on a
     * little-endian MCU (nRF52840, ARM Cortex-M4 in LE mode), so the wire
     * carries id_hash LSB-first. Its own debug log prints `%08X`, i.e.
     * MSB-first TEXT. The plan doc's "Byte-order trap" ratifies the canonical
     * form as THE UINT32 AS PRINTED, so:
     *
     *   wire bytes  12 EE FF C0   -> sys_get_le32() -> 0xC0FFEE12
     *   panel shows "ID C0FFEE12"                      ^ same digits
     *   user pastes 0xC0FFEE12 into the allowlist      ^ same digits
     *
     * sys_get_le32() rather than a struct memcpy: memcpy would be correct only
     * by accident of this MCU's endianness and would break silently on a
     * big-endian host build of the unit test. See the report note -- this is
     * reasoned from the plan doc, NOT read from upstream source, because the
     * broadcaster module is not vendored in this workspace.
     */
    out->keyboard_id = sys_get_le32(&data[DISPSCAN_OFF_KEYBOARD_ID]);

    /*
     * TRAP #5 — CHARGING (0x02) IS NOT DECODED. The bit exists in the upstream
     * header and no code anywhere sets it, so it is always 0 on air. Surfacing
     * it would create a permanently-false input that some later widget starts
     * depending on. Add it together with a keyboard-side writer, not before.
     */
    flags = data[DISPSCAN_OFF_STATUS_FLAGS];
    out->caps_word = (flags & DISPSCAN_WIRE_FLAG_CAPS_WORD) != 0;
    out->usb_connected = (flags & DISPSCAN_WIRE_FLAG_USB_CONN) != 0;
    out->usb_hid_ready = (flags & DISPSCAN_WIRE_FLAG_USB_HID) != 0;
    out->ble_connected = (flags & DISPSCAN_WIRE_FLAG_BLE_CONN) != 0;
    out->ble_bonded = (flags & DISPSCAN_WIRE_FLAG_BLE_BONDED) != 0;

    /*
     * TRAP #4 — L AND R ARE THE SAME INFORMATION. The writer sets BOTH bits of
     * a class whenever that class is held, so a decoder that reported them
     * separately would be inventing data. OR them into one flag per class.
     */
    mods = data[DISPSCAN_OFF_MODIFIER_FLAGS];
    out->modifiers = 0;
    if (mods & (DISPSCAN_WIRE_MOD_LCTL | DISPSCAN_WIRE_MOD_RCTL)) {
        out->modifiers |= DISPSCAN_MOD_CTRL;
    }
    if (mods & (DISPSCAN_WIRE_MOD_LSFT | DISPSCAN_WIRE_MOD_RSFT)) {
        out->modifiers |= DISPSCAN_MOD_SHIFT;
    }
    if (mods & (DISPSCAN_WIRE_MOD_LALT | DISPSCAN_WIRE_MOD_RALT)) {
        out->modifiers |= DISPSCAN_MOD_ALT;
    }
    if (mods & (DISPSCAN_WIRE_MOD_LGUI | DISPSCAN_WIRE_MOD_RGUI)) {
        out->modifiers |= DISPSCAN_MOD_GUI;
    }

    out->wpm = data[DISPSCAN_OFF_WPM];

    /*
     * DELIBERATELY DROPPED, each for a stated reason:
     *
     *   offset 8  connection_count — hardcoded 1, +1 if USB HID is ready. A
     *                                strictly worse encoding of usb_hid_ready.
     *   offset 10 device_role      — always 1 (central); peripherals do not
     *                                advertise, so it carries no information.
     *   offset 11 device_index     — unused by a single-keyboard display.
     *   offset 25 channel          — D8 rules channel out AS AN IDENTITY: the
     *                                upstream matcher treats keyboard channel 0
     *                                and scanner channel >= 10 as wildcards, so
     *                                a colleague on defaults is accepted by
     *                                every scanner. keyboard_id with strict
     *                                equality is the identity here, and adding
     *                                channel as a second, weaker filter would
     *                                only give a future edit something to
     *                                loosen. See dispscan_observer.c.
     */

    return DISPSCAN_DECODE_OK;
}

const char *dispscan_decode_result_str(enum dispscan_decode_result r) {
    switch (r) {
    case DISPSCAN_DECODE_OK:
        return "ok";
    case DISPSCAN_DECODE_TOO_SHORT:
        return "too-short";
    case DISPSCAN_DECODE_BAD_MAGIC:
        return "bad-magic";
    case DISPSCAN_DECODE_BAD_VERSION:
        return "bad-version";
    default:
        return "?";
    }
}
