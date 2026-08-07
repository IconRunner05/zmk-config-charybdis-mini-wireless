/*
 * dispscan — WIRE DECODER. 26 raw bytes in, a decoded status out.
 *
 * SPDX-License-Identifier: MIT
 *
 * THIS UNIT OWNS THE BYTE LAYOUT AND NOTHING ELSE. No radio, no LVGL, no
 * kernel objects, no static state -- dispscan_packet_decode() is a pure
 * function of (bytes, length). That is deliberate and load-bearing:
 *
 *   * it is the ONLY place a wire offset, a bit mask or a byte-order concern
 *     may appear (dispscan_status.h states the same rule from the other side);
 *   * being pure, it can be exercised without a broadcaster -- which matters a
 *     great deal right now, because NO BROADCASTER EXISTS YET. Every trap below
 *     is therefore proven by dispscan_decode_test.c feeding hand-built buffers
 *     through THIS function, not through a reimplementation of it.
 *
 * The layout is the 26-byte status advertisement in docs/remote-display-plan.md,
 * "The wire contract". Trap numbers below are that document's numbering.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "dispscan_status.h"

/*
 * On air the payload is one AD element:
 *
 *     0x1B 0xFF | FF FF AB CD ...
 *     ^len ^BT_DATA_MANUFACTURER_DATA
 *
 * Zephyr's bt_data_parse() strips the length and type bytes, so `data->data`
 * points at the 0xFF 0xFF and `data->data_len` is 26. The decoder is handed
 * exactly that slice -- it never sees the AD header.
 */
#define DISPSCAN_WIRE_LEN 26

/* Offsets, for the decoder and for the test hook that builds buffers by hand.
 * Named rather than inlined so a layout change is one visible diff. */
#define DISPSCAN_OFF_MANUFACTURER_ID 0 /* 2 bytes, 0xFF 0xFF */
#define DISPSCAN_OFF_SERVICE_UUID 2    /* 2 bytes, 0xAB 0xCD -- not a real UUID */
#define DISPSCAN_OFF_VERSION 4
#define DISPSCAN_OFF_BATTERY_LEVEL 5
#define DISPSCAN_OFF_ACTIVE_LAYER 6
#define DISPSCAN_OFF_PROFILE_SLOT 7
#define DISPSCAN_OFF_CONNECTION_COUNT 8
#define DISPSCAN_OFF_STATUS_FLAGS 9
#define DISPSCAN_OFF_DEVICE_ROLE 10
#define DISPSCAN_OFF_DEVICE_INDEX 11
#define DISPSCAN_OFF_PERIPHERAL_BATTERY 12 /* 3 bytes; [0] is the other half */
#define DISPSCAN_OFF_LAYER_NAME 15         /* 4 bytes, NOT NUL-terminated */
#define DISPSCAN_OFF_KEYBOARD_ID 19        /* 4 bytes, little-endian */
#define DISPSCAN_OFF_MODIFIER_FLAGS 23
#define DISPSCAN_OFF_WPM 24
#define DISPSCAN_OFF_CHANNEL 25

/*
 * TRAP #6 — matching is a 4-BYTE MAGIC COMPARE AND NOTHING ELSE. No UUID
 * filter (0xABCD is not a GATT UUID, it is a magic word inside the payload),
 * no address filter (BLE addresses rotate), no RSSI threshold.
 */
#define DISPSCAN_MAGIC_0 0xFFu
#define DISPSCAN_MAGIC_1 0xFFu
#define DISPSCAN_MAGIC_2 0xABu
#define DISPSCAN_MAGIC_3 0xCDu

/*
 * The one wire-format major this decoder claims to understand.
 *
 * A major bump means the offsets above moved. Decoding a v2 payload with v1
 * offsets does not fail -- every field still parses, into garbage -- so the
 * only defence is to refuse. That is the whole reason `version` exists at
 * offset 4 and the reason dispscan_status.h insists the decoder populate it.
 *
 * NO NEGOTIATION IS BUILT HERE, deliberately. There is nothing to negotiate
 * with: a broadcast has no back-channel. Reject and log; a human then rebuilds
 * one side. A MINOR bump is accepted silently -- minor is reserved for
 * additions in bytes this decoder already ignores.
 */
#define DISPSCAN_WIRE_VERSION_MAJOR 1

/* Wire status_flags bits (offset 9). CHARGING (0x02) is DELIBERATELY ABSENT --
 * trap #5: it is declared upstream and no code ever sets it. See the decoder. */
#define DISPSCAN_WIRE_FLAG_CAPS_WORD 0x01u
#define DISPSCAN_WIRE_FLAG_USB_CONN 0x04u
#define DISPSCAN_WIRE_FLAG_USB_HID 0x08u
#define DISPSCAN_WIRE_FLAG_BLE_CONN 0x10u
#define DISPSCAN_WIRE_FLAG_BLE_BONDED 0x20u

/* Wire modifier_flags bits (offset 23). Trap #4: the WRITER sets both bits of
 * each class, so L and R carry zero information and are ORed together. */
#define DISPSCAN_WIRE_MOD_LCTL 0x01u
#define DISPSCAN_WIRE_MOD_LSFT 0x02u
#define DISPSCAN_WIRE_MOD_LALT 0x04u
#define DISPSCAN_WIRE_MOD_LGUI 0x08u
#define DISPSCAN_WIRE_MOD_RCTL 0x10u
#define DISPSCAN_WIRE_MOD_RSFT 0x20u
#define DISPSCAN_WIRE_MOD_RALT 0x40u
#define DISPSCAN_WIRE_MOD_RGUI 0x80u

/**
 * Why a decode was refused. Distinct values rather than a bool because the
 * three failures want three different reactions from a human:
 *   TOO_SHORT / BAD_MAGIC — some other device's manufacturer data. Expected,
 *                           constant, uninteresting; log at DBG at most.
 *   BAD_VERSION           — OUR keyboard, speaking a layout we do not know.
 *                           Loud: it means one of the two images needs a
 *                           rebuild, and it is the only failure a user can fix.
 */
enum dispscan_decode_result {
    DISPSCAN_DECODE_OK = 0,
    DISPSCAN_DECODE_TOO_SHORT,
    DISPSCAN_DECODE_BAD_MAGIC,
    DISPSCAN_DECODE_BAD_VERSION,
};

/**
 * Decode one manufacturer-data payload.
 *
 * @param data  manufacturer data, starting at the 0xFF 0xFF company ID.
 * @param len   its length. Payloads LONGER than 26 are accepted: a future
 *              minor version may append, and the fields we read are unmoved.
 * @param out   filled only on DISPSCAN_DECODE_OK; untouched otherwise, so a
 *              caller cannot accidentally render half a rejected packet.
 *
 * @return DISPSCAN_DECODE_OK, or why not.
 *
 * @note `out->link`, `out->freshness` and `out->rssi` are NOT set. Liveness is
 *       a function of TIME and signal strength is a property of the RADIO, and
 *       this function has access to neither. dispscan_link.c owns the first
 *       two; the observer supplies the third. Keeping them out is what stops
 *       the decoder from quietly becoming the state machine.
 */
enum dispscan_decode_result dispscan_packet_decode(const uint8_t *data, size_t len,
                                                   struct dispscan_status *out);

/** Human-readable form of a decode result, for logging. Never NULL. */
const char *dispscan_decode_result_str(enum dispscan_decode_result r);
