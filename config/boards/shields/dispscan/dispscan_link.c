/*
 * dispscan — link state machine.
 *
 * SPDX-License-Identifier: MIT
 *
 * See dispscan_link.h for why this is a separate unit and for the push-only
 * seam problem the timer below solves.
 */

#include <string.h>

#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include "dispscan_link.h"
#include "dispscan_status.h"

/*
 * ----------------------------------------------------------------------------
 * State
 *
 * Touched from TWO contexts and therefore mutex-guarded:
 *   * the BT RX thread, via dispscan_link_on_packet() from the scan callback;
 *   * the system work queue, via the free-running tick below.
 * Both are threads, never ISRs — see the k_work_delayable note at the bottom
 * for why the timer is a delayable work item rather than a k_timer.
 * ----------------------------------------------------------------------------
 */
K_MUTEX_DEFINE(dispscan_link_mutex);

static struct {
    /** Last successfully decoded payload from the bound keyboard. Held across
     *  silence on purpose: D8 is explicit that last-known values are never
     *  zeroed, because a battery reading of 0% is worse than a stale one. */
    struct dispscan_status data;
    /** False until the first packet ever. Distinguishes "nothing heard" from
     *  "an all-zero payload heard", which look identical in `data`. */
    bool have_data;

    /** k_uptime_get() of the last accepted packet. */
    int64_t last_rx;
    /** k_uptime_get() of the last packet that arrived at the ACTIVE cadence,
     *  i.e. the most recent moment we believe a human was typing. */
    int64_t last_active;

    /** What we last pushed, so the tick can push only on change. */
    enum dispscan_link_state pushed_link;
    enum dispscan_freshness pushed_fresh;
    bool pushed_valid;
} st;

static struct k_work_delayable tick_work;
static bool started;

/*
 * ----------------------------------------------------------------------------
 * AXIS 2 — inferring keyboard activity from advertising CADENCE
 *
 * The broadcaster switches between a ~1 s ACTIVE interval and a ~30 s IDLE
 * interval (D6 signal S1). D9 makes this usable: with the display USB-powered
 * we scan at 100% duty, so essentially every beacon is caught and a gap is
 * real rather than a missed window. The plan doc's k-of-window test and its
 * ~10 s evidence lag existed only to disambiguate "idle" from "missed it" at
 * reduced duty, and are not needed here.
 *
 * So: an inter-arrival gap <= CONFIG_DISPSCAN_ACTIVE_GAP_MS means the keyboard
 * is still on its active cadence. Default 3000 ms = 3x the 1 s active interval,
 * which tolerates two consecutive dropped adverts before misreading an active
 * keyboard as idle. It must stay comfortably BELOW the 30 s idle cadence or the
 * two are indistinguishable.
 *
 * THE FIRST PACKET AFTER SILENCE IS TREATED AS ACTIVE regardless of the gap it
 * followed. That is a deliberate asymmetry: D6 requires that "waking must be
 * immediate on the first active-cadence packet", and a strict gap test would
 * need TWO closely-spaced packets — i.e. an extra second of black screen every
 * time someone touches the keyboard. The cost is that the first idle-cadence
 * beacon after a long silence also reads as active; it is corrected 3 s later
 * when no follow-up arrives, and CONFIG_DISPSCAN_DARK_MS is far longer than
 * that, so it never reaches the panel.
 * ----------------------------------------------------------------------------
 */

/* Recompute both axes from the clock. Caller holds the mutex. */
static void recompute(int64_t now, enum dispscan_link_state *link_out,
                      enum dispscan_freshness *fresh_out) {
    int64_t since_rx;
    int64_t since_active;
    enum dispscan_freshness fresh;

    if (!st.have_data) {
        /* Nothing has ever been heard. NO_SIGNAL is the honest boot state and
         * the renderer already starts there; saying it again costs nothing. */
        *fresh_out = DISPSCAN_FRESH_LOST;
        *link_out = DISPSCAN_LINK_NO_SIGNAL;
        return;
    }

    since_rx = now - st.last_rx;
    since_active = now - st.last_active;

    if (since_rx < CONFIG_DISPSCAN_LIVE_MS) {
        fresh = DISPSCAN_FRESH_LIVE;
    } else if (since_rx < CONFIG_DISPSCAN_LOST_MS) {
        fresh = DISPSCAN_FRESH_IDLE;
    } else {
        fresh = DISPSCAN_FRESH_LOST;
    }

    *fresh_out = fresh;

    /*
     * RESOLUTION ORDER, and it is not arbitrary. NO_SIGNAL wins over DARK
     * whenever both apply, because "the keyboard is gone" is strictly more
     * important than "the keyboard is idle" and D8 forbids the user ever
     * mistaking one for the other. A dead keyboard is also, trivially, an
     * inactive one — so without this ordering every death would render as
     * DARK, i.e. as a normal idle screen, and the failure would be invisible.
     */
    if (fresh == DISPSCAN_FRESH_LOST) {
        *link_out = DISPSCAN_LINK_NO_SIGNAL;
    } else if (since_active >= CONFIG_DISPSCAN_DARK_MS) {
        *link_out = DISPSCAN_LINK_DARK;
    } else {
        *link_out = DISPSCAN_LINK_AWAKE;
    }
}

/*
 * Recompute, and push to the renderer if anything the panel can see moved.
 *
 * @param force push even if neither axis changed — used on packet arrival,
 *              where the DATA may have changed while the state did not.
 *
 * Pushes OUTSIDE this unit's mutex. dispscan_status_update() takes its own
 * mutex (custom_status_screen.c), and holding two locks across a call into
 * another module is how lock-order bugs are built. Copying the snapshot out
 * first costs one struct copy on a path that runs at most once per second.
 */
static void evaluate_and_push(bool force) {
    struct dispscan_status snapshot;
    enum dispscan_link_state link;
    enum dispscan_freshness fresh;
    bool changed;

    k_mutex_lock(&dispscan_link_mutex, K_FOREVER);

    recompute(k_uptime_get(), &link, &fresh);

    changed = !st.pushed_valid || link != st.pushed_link || fresh != st.pushed_fresh;

    if (changed || force) {
        snapshot = st.data;
        snapshot.link = link;
        snapshot.freshness = fresh;

        st.pushed_link = link;
        st.pushed_fresh = fresh;
        st.pushed_valid = true;
    }

    k_mutex_unlock(&dispscan_link_mutex);

    if (!(changed || force)) {
        return;
    }

    if (changed) {
        LOG_INF("dispscan: link=%d freshness=%d", (int)link, (int)fresh);
    }

    dispscan_status_update(&snapshot);
}

/*
 * ----------------------------------------------------------------------------
 * THE FREE-RUNNING TICK — the reason this file exists.
 *
 * Reschedules itself unconditionally, so it keeps running through total radio
 * silence. That is the whole point: a dead keyboard produces NO callbacks, so
 * the only component that can notice it is one that runs when nothing happens.
 *
 * k_work_delayable on the SYSTEM work queue, not a k_timer. A k_timer expiry
 * function runs in ISR context, and both dispscan_link_mutex and the mutex
 * inside dispscan_status_update() are illegal there (dispscan_status.h says so
 * explicitly). A k_timer would therefore have needed a k_work behind it
 * anyway; this is the same thing with one fewer object and no ISR to reason
 * about.
 *
 * NOT the display work queue: that one is dedicated to LVGL
 * (nice_view's Kconfig.defconfig selects ZMK_DISPLAY_WORK_QUEUE_DEDICATED), and
 * dispscan_status_update() bounces onto it by itself. Submitting here too would
 * serialise state evaluation behind frame rendering.
 *
 * Tick period is a fraction of the shortest threshold it has to detect
 * (CONFIG_DISPSCAN_LIVE_MS, default 5 s), so worst-case detection lag is one
 * tick. At 1 Hz on a mains-powered device the cost is not worth optimising.
 * ----------------------------------------------------------------------------
 */
static void tick_cb(struct k_work *work) {
    ARG_UNUSED(work);

    evaluate_and_push(false);

    k_work_schedule(&tick_work, K_MSEC(CONFIG_DISPSCAN_TICK_MS));
}

void dispscan_link_start(void) {
    if (started) {
        return;
    }
    started = true;

    k_work_init_delayable(&tick_work, tick_cb);

    /* Immediately, not after one period: the first evaluation establishes
     * NO_SIGNAL as a PUSHED state rather than merely the renderer's boot
     * default, so `pushed_valid` is true before any packet can race it. */
    k_work_schedule(&tick_work, K_NO_WAIT);

    LOG_INF("dispscan: link state machine started "
            "(live<%dms idle<%dms dark>=%dms tick=%dms active-gap<=%dms)",
            CONFIG_DISPSCAN_LIVE_MS, CONFIG_DISPSCAN_LOST_MS, CONFIG_DISPSCAN_DARK_MS,
            CONFIG_DISPSCAN_TICK_MS, CONFIG_DISPSCAN_ACTIVE_GAP_MS);
}

void dispscan_link_on_packet(const struct dispscan_status *decoded, int8_t rssi) {
    int64_t now;
    int64_t gap;

    if (decoded == NULL) {
        return;
    }

    k_mutex_lock(&dispscan_link_mutex, K_FOREVER);

    now = k_uptime_get();
    gap = st.have_data ? (now - st.last_rx) : 0;

    st.data = *decoded;
    st.data.rssi = rssi;

    /*
     * THE CADENCE TEST — axis 2, and the only place it is applied.
     *
     * `gap <= ACTIVE_GAP_MS` is the real signal: packets still arriving at the
     * ~1 s active interval mean a human is typing. An idle-cadence packet
     * (~30 s gap) deliberately does NOT refresh last_active — if it did,
     * last_active would track last_rx, since_active would never grow, and DARK
     * COULD NEVER FIRE while any beacon at all was arriving. That is the whole
     * mechanism; do not "simplify" this to an unconditional assignment.
     *
     * Two cases are also counted as active, both to avoid rendering a lie:
     *   * `!st.have_data` — the very first packet. We have no gap to measure
     *     and no reason to assume the keyboard is idle.
     *   * coming back from FRESH_LOST — the keyboard was gone and is back. Its
     *     first packet would otherwise be scored on a gap of minutes, so the
     *     panel would go NO_SIGNAL -> DARK -> AWAKE, flashing an idle screen at
     *     the exact moment the user is looking for confirmation it returned.
     *
     * Consequence, stated plainly: waking from DARK costs TWO packets (~2 s at
     * the 1 s active interval), because the first one after a long gap is
     * scored as idle cadence. D6 asks for immediate wake; two beacons is the
     * floor for a cadence-based inference and the honest cost of not owning
     * the broadcaster. A dedicated ACTIVE bit (D6 signal S4) collapses it to
     * one packet, and this is the single line that would change.
     */
    if (!st.have_data || st.pushed_fresh == DISPSCAN_FRESH_LOST ||
        gap <= CONFIG_DISPSCAN_ACTIVE_GAP_MS) {
        st.last_active = now;
    }

    st.last_rx = now;
    st.have_data = true;

    k_mutex_unlock(&dispscan_link_mutex);

    /* force=true: the state axes may be unchanged while layer, battery or
     * modifiers moved, and those are exactly what the panel is for. */
    evaluate_and_push(true);
}
