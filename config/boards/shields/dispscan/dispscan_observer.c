/*
 * dispscan — BLE observer implementation.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include "dispscan_link.h"
#include "dispscan_observer.h"
#include "dispscan_packet.h"
#include "dispscan_status.h"

/*
 * ============================== SCAN PARAMETERS ==============================
 *
 * PASSIVE, per the plan doc's Phase 4 facts and D8. The upstream reference
 * scans ACTIVELY, which is wrong here for two independent reasons:
 *   * an active scanner TRANSMITS a SCAN_REQ per advertisement, so every
 *     display would cost the keyboard a SCAN_RSP transmission — turning
 *     "one keyboard, many displays is free" into a per-display battery tax on
 *     the half already carrying the heaviest radio load (D7);
 *   * the only thing active scanning buys is the SCAN_RSP, which carries
 *     BT_DATA_NAME_COMPLETE. We identify keyboards by `keyboard_id`, not by
 *     name (D8: names are not stable, IDs are), so the response is pure cost.
 *
 * 100% DUTY: interval == window == 0x0030 (30 ms), per D9 — the display is
 * USB-powered, scanner power is a non-goal, and the scan-duty state machine
 * that D6 originally called for is deliberately absent. These are Zephyr's own
 * BT_GAP_SCAN_FAST_INTERVAL_MIN and BT_GAP_SCAN_FAST_WINDOW, which
 * zephyr/include/zephyr/bluetooth/bluetooth.h asserts are equal precisely so
 * they can be used for continuous scanning.
 *
 * BT_LE_SCAN_OPT_NONE — *NOT* BT_LE_SCAN_OPT_FILTER_DUPLICATE, and this is the
 * trap in this file. Every stock BT_LE_SCAN_PASSIVE* helper macro sets
 * FILTER_DUPLICATE. Under it the controller reports each distinct advertiser
 * ONCE and then suppresses identical repeats — which would delete the entire
 * input to dispscan_link.c's cadence inference (a keyboard sending the same
 * payload every second would be heard once), and would make silence
 * indistinguishable from a steady state. We need EVERY beacon, so no helper
 * macro is usable and the parameters are spelled out.
 */
static const struct bt_le_scan_param scan_param = {
    .type = BT_LE_SCAN_TYPE_PASSIVE,
    .options = BT_LE_SCAN_OPT_NONE,
    .interval = BT_GAP_SCAN_FAST_INTERVAL_MIN, /* 0x0030 = 30 ms */
    .window = BT_GAP_SCAN_FAST_WINDOW,         /* 0x0030 = 30 ms -> 100% duty */
    .timeout = 0,
};

/*
 * ================== D8 — WHICH KEYBOARD ARE WE LOOKING AT? ==================
 *
 * PRIMARY: a compile-time allowlist of `keyboard_id` values, enforced as a
 * HARD REJECT here. Strict equality, no wildcards, no escape hatch.
 *
 * Upstream's channel matcher is NOT reused and must not be reintroduced. Its
 * rule is `scanner_ch == 0 || scanner_ch >= 10 || kb_ch == 0 || scanner_ch ==
 * kb_ch`, which has two defects: `kb_ch == 0` is a WILDCARD HELD BY THE
 * KEYBOARD (a colleague on defaults is accepted by every scanner, defeating
 * the exact scenario channels exist for), and `scanner_ch >= 10` is silently
 * promiscuous (pick "42" thinking it private, get a wide-open scanner).
 *
 * FALLBACK IS THE SAME MECHANISM, UNBOUND. An empty allowlist is DISCOVERY
 * MODE: bind to the strongest keyboard heard and render its ID, so a freshly
 * flashed device is usable out of the box and the setup procedure is "read 8
 * hex digits off the screen, paste them into the shield .conf, rebuild". Setup
 * and graceful degradation are one mechanism, not two.
 *
 * Why this over every alternative: a non-allowlisted ID never reaches display
 * state, so the only possible wrong output is NO SIGNAL — loud and correct.
 * Every other scheme can fail SILENTLY by rendering a stranger's data.
 *
 * FORMAT of CONFIG_DISPSCAN_KEYBOARD_ID_ALLOWLIST: comma-separated, base
 * auto-detected, so "0xC0FFEE12,0xDEADBEEF" and "3237998610" both work. The
 * values are the uint32 AS PRINTED by the panel's "ID %08X" — see the
 * byte-order note in dispscan_packet.c; the canonical form is deliberately the
 * printed one so the digits a user copies are the digits they paste.
 */
#define ALLOWLIST_MAX 4

static uint32_t allowlist[ALLOWLIST_MAX];
static size_t allowlist_len;

static void allowlist_parse(void) {
    const char *p = CONFIG_DISPSCAN_KEYBOARD_ID_ALLOWLIST;

    while (*p != '\0' && allowlist_len < ALLOWLIST_MAX) {
        char *end;
        unsigned long v;

        while (*p == ',' || *p == ' ') {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        v = strtoul(p, &end, 0);
        if (end == p) {
            /* Not a number. Skip to the next comma rather than spinning, and
             * say so — a typo in the .conf must not silently become discovery
             * mode, which accepts ANY keyboard. */
            LOG_ERR("dispscan: unparseable keyboard_id in allowlist at \"%s\"", p);
            while (*p != '\0' && *p != ',') {
                p++;
            }
            continue;
        }

        allowlist[allowlist_len++] = (uint32_t)v;
        p = end;
    }

    if (allowlist_len == 0) {
        LOG_WRN("dispscan: keyboard_id allowlist EMPTY -- discovery mode. The panel "
                "shows whichever keyboard is strongest. Paste the ID it displays into "
                "CONFIG_DISPSCAN_KEYBOARD_ID_ALLOWLIST and rebuild to bind it.");
    } else {
        for (size_t i = 0; i < allowlist_len; i++) {
            LOG_INF("dispscan: allowlisted keyboard_id %08X", (unsigned int)allowlist[i]);
        }
    }
}

static bool allowlisted(uint32_t id) {
    for (size_t i = 0; i < allowlist_len; i++) {
        if (allowlist[i] == id) {
            return true;
        }
    }
    return false;
}

/*
 * ------------------------- Selection / hysteresis --------------------------
 *
 * With N > 1 allowlisted keyboards (or in discovery mode with several in
 * range) exactly one is shown at a time, and it must not FLAP. D8's rule:
 * switch only when the challenger has been more recent for >= 3 consecutive
 * packets AND the incumbent has been silent >= 10 s. Cadence is preferred over
 * RSSI as the tiebreak because the board you are typing on is the one
 * advertising at the ACTIVE rate — a semantic signal rather than a physical
 * proxy — and "3 consecutive packets from the challenger while the incumbent
 * says nothing" is exactly that comparison.
 *
 * Discovery mode is the one place RSSI decides, because there the question is
 * literally "which keyboard is on this desk" and there is no user intent to
 * read. Requiring the challenger to beat the incumbent by a margin (rather
 * than merely exceed it) is what stops two similarly-placed keyboards from
 * trading the display back and forth on measurement noise.
 *
 * NOTE: with N == 1 allowlisted keyboard none of this executes — the reject
 * above has already discarded everything else. It is dead weight in the
 * default configuration and alive the moment a second ID is added.
 */
static struct {
    bool bound;
    uint32_t id;
    int8_t rssi;
    int64_t last_seen;

    uint32_t challenger;
    uint8_t challenger_hits;
} sel;

#define CHALLENGER_HITS_REQUIRED 3
#define INCUMBENT_SILENCE_MS 10000

static bool select_keyboard(uint32_t id, int8_t rssi, int64_t now) {
    if (!sel.bound) {
        sel.bound = true;
        sel.id = id;
        LOG_INF("dispscan: bound to keyboard_id %08X (rssi %d)", (unsigned int)id, (int)rssi);
        goto accept;
    }

    if (id == sel.id) {
        sel.challenger = 0;
        sel.challenger_hits = 0;
        goto accept;
    }

    /* A different keyboard. Decide whether it takes over. */
    if (sel.challenger != id) {
        sel.challenger = id;
        sel.challenger_hits = 0;
    }
    if (sel.challenger_hits < UINT8_MAX) {
        sel.challenger_hits++;
    }

    if (allowlist_len == 0) {
        /* Discovery: strongest wins, by a margin. */
        if (rssi > sel.rssi + CONFIG_DISPSCAN_DISCOVERY_RSSI_MARGIN_DB) {
            LOG_INF("dispscan: discovery switch %08X (%d dBm) -> %08X (%d dBm)",
                    (unsigned int)sel.id, (int)sel.rssi, (unsigned int)id, (int)rssi);
            sel.id = id;
            goto accept;
        }
        /* Discovery decides on RSSI alone, so the hit counter means nothing on
         * this path -- clear it rather than leaving it to accumulate to
         * UINT8_MAX. Harmless today; a trap for anyone who later adds a rule
         * that reads challenger_hits and reasonably assumes it counts only
         * where it is used. */
        sel.challenger = 0;
        sel.challenger_hits = 0;
        return false;
    }

    if (sel.challenger_hits >= CHALLENGER_HITS_REQUIRED &&
        (now - sel.last_seen) >= INCUMBENT_SILENCE_MS) {
        LOG_INF("dispscan: switching %08X -> %08X (incumbent silent %lld ms)",
                (unsigned int)sel.id, (unsigned int)id, (long long)(now - sel.last_seen));
        sel.id = id;
        goto accept;
    }

    return false;

accept:
    sel.rssi = rssi;
    sel.last_seen = now;
    sel.challenger = 0;
    sel.challenger_hits = 0;
    return true;
}

/*
 * ----------------------------- The scan callback -----------------------------
 *
 * CONTEXT: this runs on the BT RX THREAD, not in an ISR. Zephyr's
 * zephyr/subsys/bluetooth/host/scan.c:1708 calls le_adv_recv() from the HCI
 * event handler, which the RX thread pumps, and le_adv_recv() then walks the
 * registered bt_le_scan_cb list (scan.c:658-667). That matters because
 * everything downstream of here takes a mutex:
 *   dispscan_link_on_packet() -> dispscan_link_mutex
 *                             -> dispscan_status_update() -> pending mutex
 *                             -> k_work_submit_to_queue(display wq)
 * All legal from a thread, all illegal from an ISR. The final hand-off to
 * LVGL is the display work queue, exactly as custom_status_screen.c already
 * does it — no LVGL call ever happens on this thread.
 *
 * KEEP IT SHORT ANYWAY. The RX thread also drives ZMK's own connectable
 * advertising and any host connection; blocking it is a whole-device stall.
 * The work done here is a bounded parse plus a struct copy.
 *
 * The buffer is safe to consume: le_adv_recv() does net_buf_simple_save() /
 * net_buf_simple_restore() around each listener (scan.c:660-666), so
 * bt_data_parse()'s pulls do not affect the next listener.
 */
static bool ad_parse_cb(struct bt_data *data, void *user_data) {
    const struct bt_le_scan_recv_info *info = user_data;
    struct dispscan_status decoded;
    enum dispscan_decode_result res;
    int64_t now;

    if (data->type != BT_DATA_MANUFACTURER_DATA) {
        return true; /* keep walking the AD elements */
    }

    res = dispscan_packet_decode(data->data, data->data_len, &decoded);
    if (res != DISPSCAN_DECODE_OK) {
        /*
         * BAD_VERSION is the only failure worth a human's attention: it means
         * OUR magic word arrived carrying a layout we do not know, i.e. the two
         * firmware images have diverged and someone must rebuild one. The other
         * two are just "not our packet" and happen constantly in any office.
         */
        if (res == DISPSCAN_DECODE_BAD_VERSION) {
            LOG_ERR("dispscan: keyboard speaks wire major %u, this build understands %u -- "
                    "rebuild one side",
                    (unsigned int)DISPSCAN_VERSION_MAJOR(data->data[DISPSCAN_OFF_VERSION]),
                    (unsigned int)DISPSCAN_WIRE_VERSION_MAJOR);
        }
        return true;
    }

    now = k_uptime_get();

    if (allowlist_len > 0 && !allowlisted(decoded.keyboard_id)) {
        /* HARD REJECT — never reaches display state. See the D8 block above. */
        LOG_DBG("dispscan: ignoring keyboard_id %08X (not allowlisted)",
                (unsigned int)decoded.keyboard_id);
        return false;
    }

    if (!select_keyboard(decoded.keyboard_id, info->rssi, now)) {
        return false;
    }

    dispscan_link_on_packet(&decoded, info->rssi);

    /* Our AD element is found and consumed; nothing else in this advertisement
     * is of interest (passive scanning means there is no name to collect). */
    return false;
}

static void scan_recv_cb(const struct bt_le_scan_recv_info *info, struct net_buf_simple *buf) {
    /* Cast away const only to satisfy bt_data_parse()'s void* user_data; the
     * callback re-adds it immediately and never writes through the pointer. */
    bt_data_parse(buf, ad_parse_cb, (void *)info);
}

static struct bt_le_scan_cb scan_cb = {
    .recv = scan_recv_cb,
};

int dispscan_observer_start(void) {
    int err;

    bt_le_scan_cb_register(&scan_cb);

    /* NULL callback: results are delivered through the registered
     * bt_le_scan_cb above, which bluetooth.h:2431 names as the preferred form
     * and which is the only one that hands us a bt_le_scan_recv_info (and
     * therefore an RSSI in a struct we can pass around). */
    err = bt_le_scan_start(&scan_param, NULL);
    if (err) {
        /* Unregister before returning. Leaving the callback registered against
         * a scan that never started strands it: nothing would ever invoke it,
         * and a later retry would call bt_le_scan_cb_register() a second time
         * on the same node -- Zephyr keeps registered callbacks in an intrusive
         * sys_slist, so re-adding a node already on the list corrupts it. The
         * retry path below depends on this cleanup being correct. */
        bt_le_scan_cb_unregister(&scan_cb);
        LOG_ERR("dispscan: bt_le_scan_start failed (%d) -- retrying; panel stays on NO SIGNAL", err);
        return err;
    }

    LOG_INF("dispscan: passive scan started, %u/%u (100%% duty)", (unsigned int)scan_param.interval,
            (unsigned int)scan_param.window);
    return 0;
}

/*
 * ============================ WHEN TO START ============================
 *
 * ZMK OWNS bt_enable(). It is called from exactly one place in the tree,
 * zmk/app/src/ble.c:733 inside zmk_ble_init(), which is registered as
 * `SYS_INIT(zmk_ble_init, APPLICATION, CONFIG_ZMK_BLE_INIT_PRIORITY)`
 * (ble.c:834, priority default 50 per app/Kconfig:507-509). Calling
 * bt_enable() ourselves would return -EALREADY at best.
 *
 * THERE IS NO PUBLIC "BLE IS READY" SIGNAL TO SUBSCRIBE TO. Verified rather
 * than assumed: ble.c's own readiness hook `zmk_ble_ready()` is `static`
 * (ble.c:682) and is called directly from zmk_ble_complete_startup()
 * (ble.c:727); it raises no ZMK event and exports no callback. A grep for
 * `zmk_ble_ready` across app/src and app/include finds those two lines and
 * nothing else.
 *
 * What makes ordering sufficient is that ZMK passes NULL to bt_enable(), which
 * is Zephyr's SYNCHRONOUS form — it does not return until the stack is up. So
 * a SYS_INIT at the same level with a LATER priority number is guaranteed to
 * run after Bluetooth is ready. +1 rather than a large offset, so ZMK raising
 * its own priority cannot silently overtake us.
 *
 * bt_is_ready() (zephyr/include/zephyr/bluetooth/bluetooth.h:256) is checked
 * anyway rather than trusted: if a future ZMK makes init asynchronous, the
 * failure mode becomes a logged error and a retry instead of a scan that never
 * starts and says nothing.
 */
static void start_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(start_work, start_work_cb);

/*
 * Retry cadence. The fast phase covers the expected case (a future async
 * bt_enable() taking a moment); the slow phase exists because LATCHING OFF
 * PERMANENTLY IS THE WRONG FAILURE MODE for this device.
 *
 * A scanner that gave up 10 seconds after boot looks identical, forever, to a
 * scanner whose keyboard is switched off: both show NO SIGNAL. There is no
 * front panel, no key to press and no host to complain to, so the only way out
 * would be a power cycle the user has no reason to suspect is needed. Retrying
 * slowly costs one work-queue wakeup per 5 s and makes the failure recoverable
 * on its own.
 */
#define START_RETRY_MS 500
#define START_RETRY_SLOW_MS 5000
#define START_RETRY_FAST_MAX 20

static int start_attempts;

static void start_work_cb(struct k_work *work) {
    ARG_UNUSED(work);

    k_timeout_t backoff =
        (start_attempts < START_RETRY_FAST_MAX) ? K_MSEC(START_RETRY_MS) : K_MSEC(START_RETRY_SLOW_MS);

    if (!bt_is_ready()) {
        /* Log once at the fast/slow boundary rather than every tick: by this
         * point something is genuinely wrong and a 5 s heartbeat of identical
         * errors would bury whatever else the console has to say. */
        if (++start_attempts == START_RETRY_FAST_MAX) {
            LOG_ERR("dispscan: Bluetooth not ready after %d ms; retrying every %d ms",
                    START_RETRY_FAST_MAX * START_RETRY_MS, START_RETRY_SLOW_MS);
        }
        k_work_schedule(&start_work, backoff);
        return;
    }

    if (dispscan_observer_start() != 0) {
        /* dispscan_observer_start() has already unregistered its callback, so
         * rescheduling re-enters cleanly rather than double-registering. */
        start_attempts++;
        k_work_schedule(&start_work, backoff);
    }
}

static int dispscan_observer_init(void) {
    allowlist_parse();

    /* Start the liveness machine FIRST, and unconditionally. It must run even
     * if scanning never starts: a scanner whose radio failed to come up should
     * say NO SIGNAL, which is a state only the free-running tick can push. */
    dispscan_link_start();

    k_work_schedule(&start_work, K_NO_WAIT);

    return 0;
}

SYS_INIT(dispscan_observer_init, APPLICATION, CONFIG_DISPSCAN_OBSERVER_INIT_PRIORITY);
