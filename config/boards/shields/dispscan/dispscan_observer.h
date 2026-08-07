/*
 * dispscan — BLE OBSERVER. The radio half.
 *
 * SPDX-License-Identifier: MIT
 *
 * Owns exactly three things and delegates everything else:
 *   1. starting a passive scan once ZMK's BLE stack is up;
 *   2. finding the manufacturer-data AD element in each advertisement;
 *   3. the D8 keyboard_id allowlist / discovery decision, which is the only
 *      job here that needs RSSI.
 *
 * It owns NO byte offsets (dispscan_packet.c) and NO liveness state
 * (dispscan_link.c). If a wire offset or a timeout ever appears in
 * dispscan_observer.c, the split has been undone.
 */

#pragma once

/**
 * Start scanning. Called automatically from a SYS_INIT sequenced after ZMK's
 * own BLE init; exposed so a future power slice can stop and restart it.
 *
 * @return 0, or a negative errno from bt_le_scan_start().
 */
int dispscan_observer_start(void);
