/*
 * dispscan — LINK STATE. Turns a stream of decoded packets (and, critically,
 * the ABSENCE of one) into the two state axes the renderer needs.
 *
 * SPDX-License-Identifier: MIT
 *
 * WHY THIS IS ITS OWN UNIT. The plan doc's review, deferred finding #6:
 * *"Decoding 26 bytes and inferring keyboard state are two jobs; do not fuse
 * them into the observer file."* Decoding is a pure function of bytes
 * (dispscan_packet.c); this is a stateful function of TIME. Fusing them
 * produces a file where you cannot test either half, and — worse — one where
 * the timer below is easy to forget, because the natural observer shape has no
 * place to put it.
 *
 * ================== THE ONE THING THIS UNIT EXISTS FOR ==================
 *
 * THE SEAM IS PUSH-ONLY. dispscan_status_update() is the only way to reach the
 * renderer, and the natural observer is *scan callback -> decode -> update*.
 * Under that shape, when the keyboard dies the scan callback simply STOPS BEING
 * CALLED. Nothing pushes. The renderer is never told. The panel holds a
 * live-looking AWAKE screen, with plausible batteries and a plausible layer,
 * FOREVER — which is verbatim the failure the whole remote-display design
 * exists to prevent (docs/remote-display-plan.md, deferred finding #1).
 *
 * The fix is not clever and must not be removed: a FREE-RUNNING TIMER that
 * re-evaluates state and pushes on its own, independent of packet arrival.
 * dispscan_link_start() starts it. Silence is an input, and the only component
 * that can observe silence is one that runs when nothing happens.
 */

#pragma once

#include <stdint.h>

#include "dispscan_status.h"

/**
 * Start the free-running state timer. Idempotent.
 *
 * Until this is called nothing will ever be pushed to the renderer, which boots
 * into NO_SIGNAL — correct, but permanently so.
 */
void dispscan_link_start(void);

/**
 * Feed one accepted packet.
 *
 * @param decoded a successfully decoded status. `link`, `freshness` and `rssi`
 *                in it are ignored and overwritten — this unit owns the first
 *                two and takes the third from @p rssi.
 * @param rssi    signal strength of the advertisement that carried it, dBm.
 *
 * "Accepted" means the observer has already applied the D8 keyboard_id
 * allowlist. This unit does no filtering: it would have no way to tell a
 * second keyboard's packet from the bound one's, and mixing two keyboards'
 * timestamps into one liveness clock would make a dead keyboard look alive
 * whenever a neighbour beacons.
 *
 * Safe to call from a thread (the BT RX thread is where the observer calls it).
 * NOT safe from an ISR: it takes a mutex, and so does dispscan_status_update().
 */
void dispscan_link_on_packet(const struct dispscan_status *decoded, int8_t rssi);
