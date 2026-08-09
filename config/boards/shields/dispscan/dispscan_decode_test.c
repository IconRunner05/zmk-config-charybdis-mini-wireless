/*
 * dispscan — ON-DEVICE DECODE SELF-TEST. Development scaffold.
 *
 * SPDX-License-Identifier: MIT
 *
 * ================ WHY THIS EXISTS: THERE IS NO BROADCASTER ================
 *
 * Phase 3 (the keyboard-side broadcaster) is gated behind a Phase 2 soak that
 * nobody has started (plan doc, D10). So NOT ONE REAL PACKET HAS EVER BEEN
 * DECODED, and the entire decoder — six documented traps, a version gate and a
 * byte-order decision that is a silent permanent NO SIGNAL if wrong — is
 * currently unexercised code shipping to hardware.
 *
 * This file closes that gap the only way available: hand-built byte buffers
 * fed through THE REAL dispscan_packet_decode(), logged over the USB CDC
 * console that dispscan.conf enables. It is not a substitute for on-air
 * validation, it is what makes on-air validation a one-variable experiment
 * instead of a two-variable one — when a real beacon finally arrives and the
 * panel is wrong, this rules the decoder out first.
 *
 * IT IMPORTS NOTHING FROM THE DECODER BUT ITS PUBLIC FUNCTION. The buffers are
 * built from the OFFSET MACROS in dispscan_packet.h, not from a copy of the
 * layout, so a layout change moves both sides together and the test cannot
 * silently agree with a bug.
 *
 * Delete with CONFIG_DISPSCAN_DECODE_SELFTEST=n once a broadcaster exists.
 */

#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include "dispscan_packet.h"
#include "dispscan_status.h"

/*
 * RUN LATE, NOT AT SYS_INIT. app/Kconfig sets
 * CONFIG_LOG_PROCESS_THREAD_STARTUP_DELAY_MS=1000 under ZMK_USB_LOGGING, and
 * nothing reaches the host until the CDC ACM endpoint has enumerated — which
 * takes a second or two after boot and depends on the host. A SYS_INIT-time
 * LOG_INF burst is buffered, and if the log buffer wraps before enumeration it
 * is simply lost, i.e. the self-test would silently produce no output on
 * exactly the machine it is meant to inform. Three seconds is comfortably past
 * enumeration on every host tried and still inside the first glance at the
 * console.
 *
 * Three seconds turned out NOT to be enough on its own, though — not because of
 * enumeration, but because re-triggering the burst requires a reset that
 * re-enumerates the port the host is reading. See the note at the bottom of
 * selftest_cb(); the fix is that it repeats.
 */
#define SELFTEST_DELAY_MS 3000

/* Re-run interval. Long enough not to drown the console (the observer and the
 * link machine log here too), short enough that attaching a terminal at random
 * never waits long for a full result set. */
#define SELFTEST_REPEAT_MS 20000

/* A canonical, entirely valid v1.0 payload. Every case below is this buffer
 * with one field vandalised, so a failure names the field that broke it. */
static void build_baseline(uint8_t buf[DISPSCAN_WIRE_LEN]) {
    memset(buf, 0, DISPSCAN_WIRE_LEN);

    buf[DISPSCAN_OFF_MANUFACTURER_ID + 0] = DISPSCAN_MAGIC_0;
    buf[DISPSCAN_OFF_MANUFACTURER_ID + 1] = DISPSCAN_MAGIC_1;
    buf[DISPSCAN_OFF_SERVICE_UUID + 0] = DISPSCAN_MAGIC_2;
    buf[DISPSCAN_OFF_SERVICE_UUID + 1] = DISPSCAN_MAGIC_3;

    buf[DISPSCAN_OFF_VERSION] = (DISPSCAN_WIRE_VERSION_MAJOR << 4) | 0x0; /* v1.0 */

    buf[DISPSCAN_OFF_BATTERY_LEVEL] = 87;         /* central half */
    buf[DISPSCAN_OFF_PERIPHERAL_BATTERY + 0] = 42; /* other half */

    buf[DISPSCAN_OFF_ACTIVE_LAYER] = 2;

    /*
     * TRAP #1 ON PURPOSE. 0x10 | 3 is exactly what current keyboard firmware
     * puts on the wire for profile 3: patch=2 in bits [5:3] plus the profile in
     * [2:0]. An unmasked decoder reports 19 and the panel draws "BT?" — which
     * looks like a broken keyboard rather than a broken scanner. Baking the
     * poisoned value into the BASELINE (not into a separate "nasty" case) means
     * every other assertion in this file also fails if the mask is ever lost.
     */
    buf[DISPSCAN_OFF_PROFILE_SLOT] = 0x10 | 3;

    buf[DISPSCAN_OFF_STATUS_FLAGS] = DISPSCAN_WIRE_FLAG_CAPS_WORD | DISPSCAN_WIRE_FLAG_BLE_CONN |
                                     DISPSCAN_WIRE_FLAG_BLE_BONDED |
                                     0x02u /* CHARGING — trap #5, must be ignored */;

    /*
     * TRAP #2 ON PURPOSE. Four printable bytes and NO ROOM for a terminator.
     * A decoder that treats the wire field as a C string reads past it into
     * keyboard_id and renders "BASE" plus four bytes of ID as the layer name.
     */
    memcpy(&buf[DISPSCAN_OFF_LAYER_NAME], "BASE", 4);

    /*
     * BYTE ORDER ON PURPOSE. Little-endian on the wire, so 0xC0FFEE12 ships
     * LSB-first as 12 EE FF C0. If sys_get_le32() were ever "simplified" to a
     * memcpy on a big-endian build, or the bytes reversed here, this reads
     * 0x12EEFFC0 — and since the allowlist is compared against the PRINTED
     * form, the result is a scanner that hears its keyboard and rejects it,
     * forever, silently. Hence a deliberately asymmetric value: 0xC0FFEE12
     * byte-reversed is nothing like itself.
     */
    buf[DISPSCAN_OFF_KEYBOARD_ID + 0] = 0x12;
    buf[DISPSCAN_OFF_KEYBOARD_ID + 1] = 0xEE;
    buf[DISPSCAN_OFF_KEYBOARD_ID + 2] = 0xFF;
    buf[DISPSCAN_OFF_KEYBOARD_ID + 3] = 0xC0;

    /* TRAP #4 ON PURPOSE: LSFT set, RSFT clear. The real writer sets both, so
     * a decoder that read only the R bits would report no shift at all. */
    buf[DISPSCAN_OFF_MODIFIER_FLAGS] = DISPSCAN_WIRE_MOD_LSFT | DISPSCAN_WIRE_MOD_RGUI;

    buf[DISPSCAN_OFF_WPM] = 62;
    buf[DISPSCAN_OFF_CHANNEL] = 7; /* ignored by design — see dispscan_packet.c */
}

static void run_case(const char *name, const uint8_t *buf, size_t len,
                     enum dispscan_decode_result expect) {
    struct dispscan_status s;
    enum dispscan_decode_result got;

    /* Poisoned so "untouched on failure" is observable rather than assumed. */
    memset(&s, 0xA5, sizeof(s));

    got = dispscan_packet_decode(buf, len, &s);

    if (got != expect) {
        LOG_ERR("SELFTEST FAIL  %-22s expected %s, got %s", name,
                dispscan_decode_result_str(expect), dispscan_decode_result_str(got));
        return;
    }

    if (got != DISPSCAN_DECODE_OK) {
        LOG_INF("selftest ok    %-22s rejected: %s", name, dispscan_decode_result_str(got));
        return;
    }

    /*
     * Printed, not asserted. A human reading the console is the oracle here:
     * these are the exact values the panel will draw, so a wrong mask or a
     * swapped battery is visible in one line. Asserting would need a second
     * copy of the expected decode per case, i.e. a second place to be wrong.
     */
    LOG_INF("selftest ok    %-22s v%u.%u L%u \"%s\" prof=%u batt L=%u R=%u "
            "mods=%02X wpm=%u id=%08X caps=%d usb=%d/%d ble=%d/%d",
            name, (unsigned int)DISPSCAN_VERSION_MAJOR(s.version),
            (unsigned int)DISPSCAN_VERSION_MINOR(s.version), (unsigned int)s.active_layer,
            s.layer_name, (unsigned int)s.profile_slot, (unsigned int)s.battery_left,
            (unsigned int)s.battery_right, (unsigned int)s.modifiers, (unsigned int)s.wpm,
            (unsigned int)s.keyboard_id, (int)s.caps_word, (int)s.usb_connected,
            (int)s.usb_hid_ready, (int)s.ble_connected, (int)s.ble_bonded);
}

/* Declared before the callback because the callback reschedules itself -- see
 * the note at the end of selftest_cb(). */
static void selftest_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(selftest_work, selftest_cb);

static void selftest_cb(struct k_work *work) {
    uint8_t buf[DISPSCAN_WIRE_LEN + 4];

    ARG_UNUSED(work);

    LOG_INF("dispscan decoder self-test -- wire major %u, central side %s, %u-entry buffer",
            (unsigned int)DISPSCAN_WIRE_VERSION_MAJOR,
            IS_ENABLED(CONFIG_DISPSCAN_CENTRAL_SIDE_RIGHT) ? "RIGHT" : "LEFT",
            (unsigned int)DISPSCAN_WIRE_LEN);

    /* 1. The nominal case, already carrying the unmasked profile byte, the
     *    unterminated 4-char name and the little-endian ID. Expect:
     *    prof=3, "BASE", id=C0FFEE12, mods=0A (SHIFT|GUI), caps=1.
     *    With CENTRAL_SIDE_RIGHT: batt L=42 R=87. */
    build_baseline(buf);
    run_case("baseline v1.0", buf, DISPSCAN_WIRE_LEN, DISPSCAN_DECODE_OK);

    /* 2. Battery 0 = N/A per the wire contract, NOT a flat cell. Both halves,
     *    because the renderer draws them through different code paths. */
    build_baseline(buf);
    buf[DISPSCAN_OFF_BATTERY_LEVEL] = 0;
    buf[DISPSCAN_OFF_PERIPHERAL_BATTERY + 0] = 0;
    run_case("battery 0 = N/A", buf, DISPSCAN_WIRE_LEN, DISPSCAN_DECODE_OK);

    /* 3. Empty layer name: all four bytes zero. Must decode to "" so the
     *    renderer falls back to the bare layer number, not to garbage. */
    build_baseline(buf);
    memset(&buf[DISPSCAN_OFF_LAYER_NAME], 0, DISPSCAN_LAYER_NAME_WIRE_LEN);
    run_case("empty layer name", buf, DISPSCAN_WIRE_LEN, DISPSCAN_DECODE_OK);

    /* 4. Layer name truncated mid-codepoint — a layer called "Nav" plus a
     *    multi-byte arrow, cut at 4 bytes. The decoder must pass it through
     *    unharmed (it is a valid wire value); sanitising is the renderer's job
     *    and happens in fmt_layer(). Expect the log to show mojibake here. */
    build_baseline(buf);
    memcpy(&buf[DISPSCAN_OFF_LAYER_NAME], "Nav\xE2", 4);
    run_case("layer name non-ASCII", buf, DISPSCAN_WIRE_LEN, DISPSCAN_DECODE_OK);

    /* 5. Unclamped layer. The wire allows 0..255 despite the upstream header's
     *    "0-15" comment; "L255" is the renderer's width worst case. */
    build_baseline(buf);
    buf[DISPSCAN_OFF_ACTIVE_LAYER] = 255;
    run_case("layer 255", buf, DISPSCAN_WIRE_LEN, DISPSCAN_DECODE_OK);

    /* 6. Every modifier bit set. Must collapse to 0x0F, four classes — trap #4
     *    proves itself only if L-only and L+R give the same answer. */
    build_baseline(buf);
    buf[DISPSCAN_OFF_MODIFIER_FLAGS] = 0xFF;
    run_case("all modifiers", buf, DISPSCAN_WIRE_LEN, DISPSCAN_DECODE_OK);

    /* 7. Minor version bump. Must be ACCEPTED: minor means "same offsets". */
    build_baseline(buf);
    buf[DISPSCAN_OFF_VERSION] = (DISPSCAN_WIRE_VERSION_MAJOR << 4) | 0x9;
    run_case("minor v1.9 accepted", buf, DISPSCAN_WIRE_LEN, DISPSCAN_DECODE_OK);

    /* 8. Unknown MAJOR. Must be REJECTED. This is the case that decides whether
     *    a future layout change corrupts the panel or announces itself. */
    build_baseline(buf);
    buf[DISPSCAN_OFF_VERSION] = ((DISPSCAN_WIRE_VERSION_MAJOR + 1) << 4) | 0x0;
    run_case("unknown major", buf, DISPSCAN_WIRE_LEN, DISPSCAN_DECODE_BAD_VERSION);

    /* 9. Too short — 25 bytes of an otherwise perfect payload. Rejection must
     *    come from the LENGTH check, before the magic compare reads bytes it
     *    does not own. */
    build_baseline(buf);
    run_case("truncated to 25", buf, DISPSCAN_WIRE_LEN - 1, DISPSCAN_DECODE_TOO_SHORT);

    /* 10. Empty manufacturer data. The degenerate length case; some beacons in
     *     the wild really do send this. */
    build_baseline(buf);
    run_case("zero length", buf, 0, DISPSCAN_DECODE_TOO_SHORT);

    /* 11. Wrong magic — a real Apple company ID (0x004C), which is the single
     *     most common manufacturer payload in any room and therefore the
     *     rejection path that runs thousands of times a minute. */
    build_baseline(buf);
    buf[DISPSCAN_OFF_MANUFACTURER_ID + 0] = 0x4C;
    buf[DISPSCAN_OFF_MANUFACTURER_ID + 1] = 0x00;
    run_case("wrong company id", buf, DISPSCAN_WIRE_LEN, DISPSCAN_DECODE_BAD_MAGIC);

    /* 12. Right company, wrong service word. Catches a decoder that checks
     *     only the first two magic bytes. */
    build_baseline(buf);
    buf[DISPSCAN_OFF_SERVICE_UUID + 1] = 0xCE;
    run_case("wrong service word", buf, DISPSCAN_WIRE_LEN, DISPSCAN_DECODE_BAD_MAGIC);

    /* 13. Longer than 26. Must be ACCEPTED and decoded from the same offsets —
     *     a minor version is allowed to append. Uses the 4 spare bytes the
     *     buffer was oversized for. */
    build_baseline(buf);
    memset(&buf[DISPSCAN_WIRE_LEN], 0x5A, 4);
    run_case("30-byte payload", buf, DISPSCAN_WIRE_LEN + 4, DISPSCAN_DECODE_OK);

    LOG_INF("dispscan decoder self-test complete");

    /*
     * REPEAT, don't fire once.
     *
     * Learned the hard way on 2026-08-07: a one-shot burst N seconds after boot
     * is unobservable in practice on this device. Reading it means attaching a
     * host terminal to the USB CDC port -- but the only way to re-trigger the
     * burst is a reset, and a reset RE-ENUMERATES the very port being read. The
     * host's file descriptor does not survive that, so the window containing the
     * results is lost. The first attempt captured the boot banner, then a log
     * line truncated mid-string, then nothing until the 60 s battery poll.
     *
     * Repeating decouples "when the test runs" from "when someone is watching",
     * which is the only property that makes it usable. The cost is ~15 log lines
     * per interval on a build that is dev-only by construction (the symbol's
     * help text says to turn it off once a real broadcaster can do this job over
     * the air), and the decoder is a pure function, so re-running it is free and
     * side-effect-free.
     */
    k_work_schedule(&selftest_work, K_MSEC(SELFTEST_REPEAT_MS));
}

static int dispscan_decode_test_init(void) {
    k_work_schedule(&selftest_work, K_MSEC(SELFTEST_DELAY_MS));
    return 0;
}

SYS_INIT(dispscan_decode_test_init, APPLICATION, CONFIG_DISPSCAN_OBSERVER_INIT_PRIORITY);
