/*
 * status_adv — WIRE ENCODER LAYOUT. The keyboard-side twin of the scanner's
 * dispscan_packet.h.
 *
 * SPDX-License-Identifier: MIT
 *
 * ============================ READ THIS FIRST ============================
 * THIS FILE IS A HAND-MAINTAINED DUPLICATE.
 *
 * The decoder lives on branch `display/scanner` at
 *   config/boards/shields/dispscan/dispscan_packet.h  (+ .c)
 * on a DIFFERENT ZMK revision, in a repository whose west.yml can only pin one
 * ZMK. The two branches never merge in either direction (plan doc, D3 and
 * "Branch topology for the broadcaster"), so this layout cannot be a shared
 * file. Any change here must be applied there by hand, and the scanner's
 * dispscan_decode_test.c is the cheapest place to catch drift.
 *
 * Offsets, names and the magic below are transcribed from that decoder. They
 * are not independently derived, and they are not negotiable from this side:
 * a broadcast has no back-channel.
 * =========================================================================
 */

#pragma once

#include <stdint.h>

/*
 * On air this is one AD element:
 *
 *     0x1B 0xFF | FF FF AB CD ...
 *     ^len ^BT_DATA_MANUFACTURER_DATA
 *
 * 26 payload + 2 AD header = 28 of the 31-byte legacy budget.
 */
#define STATUS_ADV_WIRE_LEN 26

#define STATUS_ADV_OFF_MANUFACTURER_ID 0 /* 2 bytes, 0xFF 0xFF */
#define STATUS_ADV_OFF_SERVICE_UUID 2    /* 2 bytes, 0xAB 0xCD -- not a real UUID */
#define STATUS_ADV_OFF_VERSION 4
#define STATUS_ADV_OFF_BATTERY_LEVEL 5
#define STATUS_ADV_OFF_ACTIVE_LAYER 6
#define STATUS_ADV_OFF_PROFILE_SLOT 7
#define STATUS_ADV_OFF_CONNECTION_COUNT 8
#define STATUS_ADV_OFF_STATUS_FLAGS 9
#define STATUS_ADV_OFF_DEVICE_ROLE 10
#define STATUS_ADV_OFF_DEVICE_INDEX 11
#define STATUS_ADV_OFF_PERIPHERAL_BATTERY 12 /* 3 bytes; [0] is the other half */
#define STATUS_ADV_OFF_LAYER_NAME 15         /* 4 bytes, NOT NUL-terminated */
#define STATUS_ADV_OFF_KEYBOARD_ID 19        /* 4 bytes, little-endian */
#define STATUS_ADV_OFF_MODIFIER_FLAGS 23
#define STATUS_ADV_OFF_WPM 24
#define STATUS_ADV_OFF_CHANNEL 25

/*
 * The scanner's ENTIRE membership test is a compare of these four bytes
 * (dispscan trap #6): no UUID filter -- 0xABCD is a magic word inside the
 * payload, not a GATT UUID -- no address filter, no RSSI threshold. So these
 * four bytes are the only thing that makes our packet "ours".
 */
#define STATUS_ADV_MAGIC_0 0xFFu /* company ID 0xFFFF, "reserved for testing" */
#define STATUS_ADV_MAGIC_1 0xFFu
#define STATUS_ADV_MAGIC_2 0xABu
#define STATUS_ADV_MAGIC_3 0xCDu

/*
 * version byte: [7:4] major, [3:0] minor.
 *
 * The decoder REFUSES a major it does not know (its DISPSCAN_WIRE_VERSION_MAJOR
 * is 1) and accepts any minor silently. So:
 *   - bump MINOR only for additions in bytes the decoder already ignores;
 *   - bump MAJOR the moment any offset above moves, and rebuild both images.
 */
#define STATUS_ADV_WIRE_VERSION_MAJOR 1
#define STATUS_ADV_WIRE_VERSION_MINOR 0
#define STATUS_ADV_WIRE_VERSION                                                                    \
    ((uint8_t)(((STATUS_ADV_WIRE_VERSION_MAJOR & 0x0F) << 4) |                                     \
               (STATUS_ADV_WIRE_VERSION_MINOR & 0x0F)))

/*
 * status_flags (offset 9).
 *
 * CAPS_WORD IS DECLARED AND PERMANENTLY ZERO. This is not an oversight and it
 * is not a TODO that can be closed from here:
 *
 *   ZMK has no caps-word event and no caps-word accessor, at v0.2.1 OR at main.
 *   app/src/behaviors/behavior_caps_word.c keeps its state in a file-static
 *   `struct caps_word_state` and exposes nothing. Upstream
 *   (t-ogura/prospector-zmk-module) only obtains the bit by SUBSTITUTING a
 *   forked behavior_caps_word.c into the build, and even then its
 *   keyboard-side broadcaster never populates it.
 *
 *   Forking a ZMK behaviour to light one bit on a display violates D7 outright
 *   (it is not removable by flipping a symbol, and it is not confined to this
 *   directory). So the bit stays zero until ZMK upstream raises an event.
 *
 * CHARGING (0x02) is likewise absent: it exists in upstream's header with no
 * writer anywhere, and the scanner deliberately does not decode it (trap #5).
 * We do not set it either -- there is no ZMK charge-state API at v0.2.1.
 */
#define STATUS_ADV_FLAG_CAPS_WORD 0x01u /* never set -- see above */
#define STATUS_ADV_FLAG_CHARGING 0x02u  /* never set -- no writer, not decoded */
#define STATUS_ADV_FLAG_USB_CONN 0x04u
#define STATUS_ADV_FLAG_USB_HID 0x08u
#define STATUS_ADV_FLAG_BLE_CONN 0x10u
#define STATUS_ADV_FLAG_BLE_BONDED 0x20u

/*
 * modifier_flags (offset 23).
 *
 * Bit order matches ZMK's own zmk_mod_flags_t / MOD_LCTL..MOD_RGUI exactly, so
 * we forward zmk_hid_get_explicit_mods() verbatim.
 *
 * NOTE ON TRAP #4: the scanner ORs L and R together because UPSTREAM's writer
 * sets both bits of a class whenever that class is held, making L/R
 * meaningless. OUR encoder does not do that -- it emits ZMK's real per-side
 * bits. That is strictly more information than the scanner currently uses, and
 * it is forward-compatible: the scanner's OR still produces the right answer.
 * If the display ever wants to show L-vs-R it can, without a wire change.
 */
#define STATUS_ADV_MOD_LCTL 0x01u
#define STATUS_ADV_MOD_LSFT 0x02u
#define STATUS_ADV_MOD_LALT 0x04u
#define STATUS_ADV_MOD_LGUI 0x08u
#define STATUS_ADV_MOD_RCTL 0x10u
#define STATUS_ADV_MOD_RSFT 0x20u
#define STATUS_ADV_MOD_RALT 0x40u
#define STATUS_ADV_MOD_RGUI 0x80u

/* device_role (offset 10). The scanner drops this field (peripherals do not
 * advertise, so it is always 1 on any packet it can ever see) but the byte is
 * still positional and must carry the documented value. */
#define STATUS_ADV_ROLE_STANDALONE 0
#define STATUS_ADV_ROLE_CENTRAL 1
#define STATUS_ADV_ROLE_PERIPHERAL 2

/* layer_name (offset 15) is 4 bytes, ZERO-PADDED and NOT NUL-terminated --
 * "BASE" fills all four and leaves no room for a terminator (trap #2). */
#define STATUS_ADV_LAYER_NAME_LEN 4
