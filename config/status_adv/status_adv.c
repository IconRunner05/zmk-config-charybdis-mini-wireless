/*
 * status_adv — keyboard-side status broadcaster for the remote display.
 *
 * SPDX-License-Identifier: MIT
 *
 * Target: ZMK v0.2.1 / Zephyr 3.5, split CENTRAL only (the Charybdis right
 * half). See config/status_adv/Kconfig for the gate and the knobs, and
 * status_adv_wire.h for the byte layout.
 *
 *
 * =========================== THE ONE HARD PROBLEM ===========================
 *
 * There is exactly ONE legacy advertising set on this build
 * (CONFIG_BT_EXT_ADV=n, and it must stay off: bt_le_adv_start() branches on
 * IS_ENABLED(CONFIG_BT_EXT_ADV) and adv_get_legacy() would switch ZMK's OWN
 * advertiser from the static bt_dev.adv to a slot allocated per start and
 * deleted per stop). ZMK wants that set for host pairing. We want it for the
 * beacon. Only one of us can have it.
 *
 * VERIFIED IN THIS WORKSPACE, zmk v0.2.1 (241ff39) app/src/ble.c:
 *
 *   * update_advertising() is a 5-case switch on
 *     `desired_adv + CURR_ADV(advertising_status)`. When it wants CONN and its
 *     own state is NONE it runs CHECKED_OPEN_ADV(), which is:
 *
 *         err = bt_le_adv_start(ZMK_ADV_CONN_NAME, zmk_ble_ad, 3, NULL, 0);
 *         if (err) { LOG_ERR(...); return err; }
 *         advertising_status = ZMK_ADV_CONN;
 *
 *   * If somebody else already holds the set, bt_le_adv_start_legacy() returns
 *     -EALREADY (zephyr subsys/bluetooth/host/adv.c, the
 *     `atomic_test_bit(adv->flags, BT_ADV_ENABLED)` check). CHECKED_OPEN_ADV
 *     then logs and returns, leaving advertising_status at NONE.
 *
 *   * THERE IS NO RETRY. update_advertising() is only ever reached from
 *     discrete events: bt ready, settings load, connect, disconnect (via
 *     k_work), profile select, bond clear, pairing complete, set device name.
 *     No timer, no error path, nothing polls. So a single -EALREADY makes the
 *     keyboard UNPAIRABLE until some unrelated event happens to fire it again.
 *
 * That is the failure upstream hit on profile switch, and it is the reason
 * this file is written the way it is.
 *
 *
 * ---------------------------- THE RULING TAKEN ----------------------------
 *
 * We do NOT implement upstream's "connectable proxy" (mode 3 as originally
 * specified: when ZMK fails to advertise, start a connectable advertisement
 * ourselves on its behalf). It was considered and rejected on evidence:
 *
 *   Once we hold a connectable advertisement, ZMK's advertising_status says
 *   NONE while the radio says ADVERTISING. Every subsequent ZMK transition is
 *   then computed from a false premise: its next CHECKED_OPEN_ADV gets
 *   -EALREADY again, its CHECKED_ADV_STOP is never reached because the switch
 *   only stops when it believes it is advertising, and on connect ZMK's
 *   `desired NONE + CURR_ADV(NONE)` matches no case at all and does nothing.
 *   The desync is permanent and self-reinforcing, and it hands ownership of
 *   "is this keyboard pairable" to us forever. That is a strictly worse
 *   failure than the one it was meant to fix, on the half that is already
 *   under a hang investigation.
 *
 * Instead: ZMK ALWAYS WINS THE SET. Three mechanisms, in order of strength.
 *
 *  (1) WE ONLY TAKE THE SET WHEN ZMK PROVABLY DOES NOT WANT IT.
 *      Before every start we evaluate ZMK's own desired_adv expression using
 *      its two public predicates:
 *          zmk_ble_active_profile_is_open() || !zmk_ble_active_profile_is_connected()
 *      If either is true, ZMK wants to advertise and we do not touch the set.
 *
 *  (2) WE PRE-EMPT THE ONE ASYNCHRONOUS TRANSITION THAT MATTERS.
 *      Host disconnect is the case where ZMK goes from "does not want it" to
 *      "wants it" without us being consulted. ZMK's own disconnected()
 *      callback defers with k_work_submit(&update_advertising_work) -- a
 *      SYSTEM WORKQUEUE hop. We register a bt_conn_cb of our own and, on the
 *      same disconnect, submit our release work to the SAME queue. Two
 *      properties make this deterministic:
 *        - bt_conn_cb_register() PREPENDS (conn.c: `cb->_next = callback_list;
 *          callback_list = cb;`) and notify_disconnected() walks that list
 *          before the STRUCT_SECTION ones, so the LAST registrant is called
 *          FIRST. We register lazily, from our first tick, long after ZMK
 *          registers in main()->settings_load()->profiles_handler commit.
 *        - the system workqueue is FIFO, so our release runs before ZMK's
 *          update_advertising_work.
 *      We cannot stop the advertiser inline in the callback: conn callbacks
 *      run on the BT RX thread and bt_le_adv_stop() issues a synchronous HCI
 *      command. That is exactly why ZMK defers too.
 *
 *  (3) WE HOLD THE SET FOR ~5% OF THE TIME, NOT 100%.
 *      Every period we start the advertiser, let roughly one advertising event
 *      go out, and stop it again (CONFIG_CHARYBDIS_STATUS_ADV_BURST_MS). The
 *      transitions we CANNOT pre-empt -- above all zmk_ble_prof_select(), which
 *      calls update_advertising() synchronously from the behaviour thread with
 *      no hook available before it -- therefore have a ~5% chance of landing
 *      inside our window instead of a certainty.
 *
 * RESIDUAL RISK, STATED PLAINLY AND NOT ENGINEERED AWAY:
 *   A BT profile switch (or bond clear, or pairing completion) that lands
 *   inside our ~50 ms burst still gets -EALREADY, and ZMK still will not
 *   retry. The keyboard would then not advertise until the next event. In
 *   practice the user's own reaction -- pressing the profile key again -- IS
 *   that next event, and by then we have released the set, so it recovers on
 *   the second press. It is not silent-forever, but it is a real papercut and
 *   it is the price of mode 2 existing at all.
 *
 *   ELIMINATING IT WOULD REQUIRE calling ZMK's update_advertising() to force a
 *   retry. That symbol is non-static in app/src/ble.c but is declared in NO
 *   header -- not zmk/ble.h, not anywhere under app/include. Declaring it
 *   extern here would be reaching into ZMK's internals, which the brief
 *   forbids and which would silently break on any ZMK bump. Not done.
 *
 *   IF THIS MODULE STOPS RUNNING (crash, k_work starvation, symbol turned
 *   off) WHILE HOLDING THE SET, the state it leaves behind is "our
 *   non-connectable beacon is running, forever". THERE IS NO RECOVERY PATH.
 *
 *   An earlier version of this comment claimed ZMK's next CHECKED_ADV_STOP or
 *   CHECKED_OPEN_ADV would clear the set. THAT WAS FALSE, and it was the
 *   load-bearing half of the "worst case is benign" argument. Checked against
 *   the pinned tree (v0.2.1, app/src/ble.c:158-164), CHECKED_OPEN_ADV is
 *   exactly:
 *
 *       err = bt_le_adv_start(ZMK_ADV_CONN_NAME, zmk_ble_ad, ...);
 *       if (err) { LOG_ERR(...); return err; }
 *       advertising_status = ZMK_ADV_CONN;
 *
 *   -- no bt_le_adv_stop(). And CHECKED_ADV_STOP (ble.c:134-140) is only
 *   reached from the two `desired == ZMK_ADV_NONE && CURR_ADV(CONN|DIR)`
 *   cases, i.e. only when ZMK believes it is itself advertising -- which, by
 *   construction, it does not while we hold the set. So neither macro runs.
 *
 *   CONSEQUENCES OF THE STUCK-BEACON STATE, STATED PLAINLY: the keyboard
 *   becomes unpairable (every CHECKED_OPEN_ADV returns -EALREADY and ZMK does
 *   not retry), the display keeps rendering a frozen payload that still looks
 *   LIVE, and the radio runs at 100% duty against D9's power budget.
 *
 *   WHAT ACTUALLY BOUNDS THIS: nothing in software. It is accepted because the
 *   module has no allocation, no unbounded state, and exactly one delayable
 *   work item whose every return path re-arms it -- so "stops running" means
 *   "the whole system is wedged", which is already fatal on its own terms.
 *   A power cycle clears it. If that ever stops being an acceptable bound, the
 *   fix is a hardware watchdog, not a comment.
 *
 *   We still never leave a CONNECTABLE advertisement behind, and we never
 *   modify ZMK's state.
 */

#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include <zmk/activity.h>
#include <zmk/ble.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>
#include <zmk/usb.h>

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
#include <zmk/battery.h>
#include <zmk/events/battery_state_changed.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_WPM)
#include <zmk/wpm.h>
#endif

/*
 * The peripheral battery PULL. Public header, declared at
 * app/include/zmk/split/bluetooth/central.h:22 -- unlike update_advertising(),
 * which is declared in no header and is therefore off limits under D7. ZMK's
 * own central_bas_proxy.c is the other caller, which is exactly why this is the
 * right source: it is the same value the proxy serves over GATT, so what the
 * display shows and what a phone's battery viewer shows cannot disagree.
 */
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
#include <zmk/split/bluetooth/central.h>
#endif

#include "status_adv_wire.h"

LOG_MODULE_REGISTER(charybdis_status_adv, CONFIG_CHARYBDIS_STATUS_ADV_LOG_LEVEL);

/* Belt and braces. Kconfig already refuses to set the symbol on a peripheral
 * (`depends on !ZMK_SPLIT || ZMK_SPLIT_ROLE_CENTRAL`), so this should be
 * unreachable -- but a peripheral that started advertising would compete for
 * the T_IFS window it is supposed to be answering in, and that is worth a hard
 * stop rather than a comment. */
#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#error "status_adv must never be built for a split peripheral"
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#define STATUS_ADV_EXPECTED_PERIPHERALS CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS
#else
#define STATUS_ADV_EXPECTED_PERIPHERALS 0
#endif

/* ------------------------------------------------------------------------ */
/* Advertising parameters                                                   */
/* ------------------------------------------------------------------------ */

/*
 * Non-connectable, non-scannable (ADV_NONCONN_IND). No BT_DATA_FLAGS: a
 * non-connectable beacon is not required to carry them and the scanner matches
 * on manufacturer data alone, so omitting them keeps us at 28 of 31 bytes
 * instead of 31 of 31.
 *
 * BT_LE_ADV_OPT_USE_IDENTITY IS MANDATORY, and this was the single most
 * surprising thing found while writing this file. Verified in
 * zephyr/subsys/bluetooth/host/id.c, bt_id_set_adv_own_addr(): for a
 * NON-connectable advertisement WITHOUT this flag, and with EXT_ADV off, the
 * host takes the branch its own comment labels "shared random address
 * problem" and calls bt_id_set_adv_private_addr() -> set_random_address(&nrpa).
 * That
 *   (a) overwrites the controller's single shared random address -- the same
 *       register the keyboard's identity address lives in, and
 *   (b) STOPS AND RESTARTS SCANNING to do it, which on a split central is
 *       precisely the LL_CONNECT_REQ window we must not disturb.
 * With USE_IDENTITY it instead calls bt_id_set_adv_random_addr() with the
 * identity address, and set_random_address() short-circuits ("Do nothing if we
 * already have the right address") because ZMK already put it there. So our
 * beacon issues ZERO address commands and is bit-identical, address-wise, to
 * what ZMK's own advertisement does.
 *
 * Privacy cost: none incremental. With CONFIG_BT_PRIVACY off, ZMK's own
 * connectable advertisement already broadcasts this identity address
 * continuously while unpaired, and keyboard_id in the payload is a permanent
 * identifier anyway (plan doc, D8/Privacy).
 *
 * Interval 30-60 ms is the ADV EVENT spacing WITHIN a burst, not the update
 * rate -- the update rate is the burst period. It is fast so that a ~50 ms
 * burst reliably contains at least one advertising event.
 *
 * 30 ms is BELOW the 100 ms floor that BT Core 4.2 [Vol 2, Part E, 7.8.5]
 * imposes on ADV_NONCONN_IND. That floor was lifted in BT 5.0 and Zephyr's
 * valid_adv_param() enforces it CONDITIONALLY:
 *     if (bt_dev.hci_version < BT_HCI_VERSION_5_0 && interval_min < 0x00a0)
 * The nRF52840 with Zephyr's own controller reports HCI 5.x, so this passes.
 * On a controller that reported 4.x, bt_le_adv_start() would return -EINVAL,
 * we would log a warning once per period and emit nothing -- degraded, not
 * dangerous. Raise both to BT_GAP_ADV_FAST_INT_MIN_2/MAX_2 (100/150 ms) and
 * CONFIG_CHARYBDIS_STATUS_ADV_BURST_MS to 150 if that ever happens.
 *
 * HONEST COST NOTE: a 50 ms burst at 30-60 ms spacing yields one advertising
 * event, occasionally two. D9's ~19 uA figure assumes exactly one per second,
 * so the real draw may be up to about double that. Still low single-digit
 * percent of the central's baseline, but it is not exactly D9's number.
 */
#define STATUS_ADV_PARAM                                                                           \
    BT_LE_ADV_PARAM(BT_LE_ADV_OPT_USE_IDENTITY, BT_GAP_ADV_FAST_INT_MIN_1,                         \
                    BT_GAP_ADV_FAST_INT_MAX_1, NULL)

static uint8_t payload[STATUS_ADV_WIRE_LEN];

static const struct bt_data beacon_ad[] = {
    BT_DATA(BT_DATA_MANUFACTURER_DATA, payload, STATUS_ADV_WIRE_LEN),
};

#if IS_ENABLED(CONFIG_CHARYBDIS_STATUS_ADV_PIGGYBACK)
/*
 * ==================== MIRROR OF ZMK's zmk_ble_ad[] ====================
 *
 * bt_le_adv_update_data() rewrites AD **and** scan response together
 * (zephyr adv.c -> le_adv_update(): set_ad() then set_sd()). There is no
 * "scan response only" call. So to put our payload in the scan response of
 * ZMK's live connectable advertisement we must hand back its AD unchanged.
 *
 * This array is copied byte-for-byte from zmk v0.2.1 app/src/ble.c:73-79.
 * IF ZMK CHANGES zmk_ble_ad[] AND THIS DOES NOT, WE SILENTLY DOWNGRADE THE
 * KEYBOARD'S PAIRING ADVERTISEMENT -- dropping BT_DATA_FLAGS, for instance,
 * makes it undiscoverable to most hosts. Re-check this array on every ZMK
 * bump, or set CONFIG_CHARYBDIS_STATUS_ADV_PIGGYBACK=n and accept a display
 * that freezes while the keyboard is unpaired.
 *
 * The device name is deliberately absent: ZMK starts with
 * BT_LE_ADV_OPT_USE_NAME|FORCE_NAME_IN_AD, so le_adv_update() re-appends the
 * name itself from get_adv_name_type(adv) -- and returns -EINVAL if we include
 * one ourselves (its ad_has_name() check).
 */
static const struct bt_data zmk_ad_mirror[] = {
    BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE, 0xC1, 0x03),
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA_BYTES(BT_DATA_UUID16_SOME, 0x12, 0x18, /* HID Service */
                  0x0f, 0x18                       /* Battery Service */
                  ),
};
#endif /* CONFIG_CHARYBDIS_STATUS_ADV_PIGGYBACK */

/* ------------------------------------------------------------------------ */
/* State                                                                    */
/* ------------------------------------------------------------------------ */

enum tick_phase {
    /* Build the payload and, if the set is free, start the burst. */
    PHASE_EMIT,
    /* The burst has run; give the set back. */
    PHASE_BURST_END,
};

static enum tick_phase phase = PHASE_EMIT;

/* True only between our own bt_le_adv_start() and our own bt_le_adv_stop().
 * Everything that stops the advertiser checks this first, so we can never stop
 * an advertisement that ZMK started. */
static bool we_own_adv;

static bool conn_cb_registered;

static uint32_t keyboard_id;
static uint8_t peripheral_batt; /* wire offset 12 -- the OTHER half */

/* Split link tracking.
 *
 * `split_down_since` is the k_uptime_get() value at which the split stopped
 * being fully connected. There is NO sentinel -- an earlier comment here
 * described a "UINT32 forever-ago" one that the code has never implemented.
 * The value is only ever read inside split_ok(), and only when
 * `split_connected` is false, so its content while the split is up is
 * irrelevant and the zero-initialised value at boot is never consulted on a
 * path that could act on it. `split_connected` is the actual state variable;
 * `split_down_since` is just the timestamp that goes with the false case. */
static bool split_connected;
static int64_t split_down_since;

static void tick_work_handler(struct k_work *work);
static void release_work_handler(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(tick_work, tick_work_handler);
static K_WORK_DEFINE(release_work, release_work_handler);

/* ------------------------------------------------------------------------ */
/* Payload                                                                  */
/* ------------------------------------------------------------------------ */

/*
 * keyboard_id -- the display's binding key (plan doc D8), so its byte order is
 * load-bearing and permanent.
 *
 * hwinfo_get_device_id() gives 8 bytes on nRF52840 (FICR DEVICEID). We fold
 * them to 32 bits with FNV-1a. The CANONICAL FORM IS THE UINT32 AS PRINTED:
 * we log it with %08X, we write it with sys_put_le32(), and the scanner reads
 * it back with sys_get_le32(). Those three agree by construction. Get any one
 * of them backwards and the user pastes a byte-swapped ID into the display's
 * allowlist, sees NO SIGNAL, and has nothing to debug against.
 */
static void compute_keyboard_id(void) {
    uint8_t id[16];
    ssize_t len = hwinfo_get_device_id(id, sizeof(id));

    if (len <= 0) {
        /* Not fatal: a fixed ID still lets a single display bind, it just
         * cannot tell two such keyboards apart. Loud, because it silently
         * breaks D8's premise. */
        LOG_ERR("hwinfo_get_device_id() failed (%d) -- keyboard_id is not unique", (int)len);
        keyboard_id = 0xDEADBEEFu;
        return;
    }

    uint32_t h = 2166136261u; /* FNV-1a offset basis */
    for (ssize_t i = 0; i < len; i++) {
        h ^= id[i];
        h *= 16777619u;
    }
    keyboard_id = h;

    LOG_INF("status_adv keyboard_id %08X (paste this into the display allowlist)", keyboard_id);
}

static void build_payload(void) {
    uint8_t flags = 0;
    bool usb_hid = false;

    memset(payload, 0, sizeof(payload));

    payload[STATUS_ADV_OFF_MANUFACTURER_ID + 0] = STATUS_ADV_MAGIC_0;
    payload[STATUS_ADV_OFF_MANUFACTURER_ID + 1] = STATUS_ADV_MAGIC_1;
    payload[STATUS_ADV_OFF_SERVICE_UUID + 0] = STATUS_ADV_MAGIC_2;
    payload[STATUS_ADV_OFF_SERVICE_UUID + 1] = STATUS_ADV_MAGIC_3;
    payload[STATUS_ADV_OFF_VERSION] = STATUS_ADV_WIRE_VERSION;

    /*
     * ===================== THE L/R BATTERY CONVENTION =====================
     *
     * DECIDED HERE, BECAUSE THE ENCODER GETS TO DEFINE THE TRUTH. The scanner
     * flagged this as an unresolved ambiguity (dispscan_packet.c, trap #3): if
     * the keyboard pre-swaps into physical L/R order and the scanner ALSO maps
     * by side, the two bars double-swap and both stay plausible forever.
     *
     * WE EMIT ROLE ORDER, NOT PHYSICAL ORDER:
     *
     *     offset 5  = THIS DEVICE's own battery  (the advertising central)
     *     offset 12 = the OTHER half's battery   (the split peripheral)
     *
     * We do NOT consult any "which side is the central" symbol and we do not
     * swap anything. The keyboard reports what it knows -- its own reading and
     * its peer's -- and role->physical mapping is entirely the scanner's job.
     *
     * Why this way round: only the scanner renders left and right, only the
     * scanner knows its own orientation, and a swap done here would be
     * invisible on this side. One swap, in one place, on the side that can see
     * the result.
     *
     * This matches what dispscan_packet.c ALREADY implements (its reading (a)):
     * it names these central_batt / other_batt and maps them with
     * CONFIG_DISPSCAN_CENTRAL_SIDE_RIGHT. On this keyboard the central is the
     * right half, and that symbol defaults to RIGHT, so the two sides agree
     * today with no change to either.
     *
     * CONSEQUENCE, WRITTEN DOWN BECAUSE IT IS EASY TO LOSE: if this keyboard
     * is ever reconfigured so the LEFT half is the central, nothing here
     * changes and nothing here breaks -- but the scanner's
     * CONFIG_DISPSCAN_CENTRAL_SIDE_RIGHT must be flipped, or the display
     * silently swaps the two bars.
     */
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    payload[STATUS_ADV_OFF_BATTERY_LEVEL] = zmk_battery_state_of_charge();
#endif
    /*
     * PULL, DO NOT TRUST THE EVENT. Verified on air: the event-fed
     * `peripheral_batt` was 0 in 100% of 164 captured advertisements while
     * ZMK's own BAS proxy was simultaneously serving 23% for the same half, so
     * the other half's battery rendered as N/A on the panel forever.
     *
     * The root cause of the missing event was never established, and that is
     * the point: the listener and all three ZMK_SUBSCRIPTION entries were
     * confirmed present in the linked ELF, and ZMK does issue an initial
     * bt_gatt_read at discovery (central.c:664) in addition to subscribing, so
     * every explanation left standing was a timing story about somebody else's
     * event ordering. A pull has no ordering to get wrong.
     *
     * It also matches how EVERY other field in this function is sampled --
     * read the current value at tick time -- so the event-fed member was the
     * odd one out to begin with.
     *
     * -ENOTCONN (split down) and -EINVAL (bad slot) both leave the byte at 0,
     * which the wire contract already defines as N/A. Failing to a documented
     * "unknown" is correct here; the alternative is showing the last-known
     * reading of a half that may have been off for hours.
     */
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    {
        uint8_t lvl = 0;

        if (zmk_split_get_peripheral_battery_level(0, &lvl) == 0) {
            peripheral_batt = lvl;
        } else {
            peripheral_batt = 0;
        }
    }
#endif
    payload[STATUS_ADV_OFF_PERIPHERAL_BATTERY + 0] = peripheral_batt;
    /* [1] and [2] are aux slots for boards with more than two halves. 0 = N/A. */

    /*
     * Layer. highest_layer_active() returns an INDEX; layer_name() takes an
     * ID. They are different types in v0.2.x (zmk/keymap.h:
     * zmk_keymap_layer_index_t vs zmk_keymap_layer_id_t) and conflating them
     * gives the wrong label as soon as a keymap reorders layers.
     *
     * The index goes on the wire unclamped: the decoder documents the full
     * 0..255 range as legal and the renderer is sized for it.
     */
    zmk_keymap_layer_index_t layer_index = zmk_keymap_highest_layer_active();
    payload[STATUS_ADV_OFF_ACTIVE_LAYER] = (uint8_t)layer_index;

    const char *layer_name = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(layer_index));
    if (layer_name != NULL) {
        /* Exactly 4 bytes, zero-padded, NOT NUL-terminated (trap #2). memset
         * above did the padding; we must not write a terminator. */
        for (int i = 0; i < STATUS_ADV_LAYER_NAME_LEN && layer_name[i] != '\0'; i++) {
            payload[STATUS_ADV_OFF_LAYER_NAME + i] = (uint8_t)layer_name[i];
        }
    }
    /* An unnamed layer leaves four zero bytes. That is the documented
     * zero-padded form; the display falls back to the numeric layer. */

    /*
     * profile_slot. The decoder masks & 0x07 (trap #1) because upstream packs
     * a patch version into [5:3] and a dev flag into [6], which made profile 0
     * read as 0x10 and render "BT16".
     *
     * WE WRITE THE UPPER BITS AS ZERO. There is nothing we want to say in
     * them, the decoder throws them away, and leaving them zero means a naive
     * reader of these bytes -- a nRF Connect dump, a future second scanner --
     * gets the right answer without knowing about the mask.
     */
    payload[STATUS_ADV_OFF_PROFILE_SLOT] = (uint8_t)(zmk_ble_active_profile_index() & 0x07);

    /* Guarded for the same reason ZMK_BATTERY_REPORTING and ZMK_WPM are: on a
     * build with CONFIG_ZMK_USB=n these symbols do not exist and this is an
     * undefined reference at link time. It happens to build unguarded on
     * nice_nano_v2 only because ZMK_USB defaults y there. Both flags stay
     * clear when USB is compiled out, which the decoder already renders as
     * "no USB" -- the honest answer for a build with no USB stack. */
#if IS_ENABLED(CONFIG_ZMK_USB)
    usb_hid = zmk_usb_is_hid_ready();
    if (zmk_usb_is_powered()) {
        flags |= STATUS_ADV_FLAG_USB_CONN;
    }
    if (usb_hid) {
        flags |= STATUS_ADV_FLAG_USB_HID;
    }
#endif
    if (zmk_ble_active_profile_is_connected()) {
        flags |= STATUS_ADV_FLAG_BLE_CONN;
    }
    if (!zmk_ble_active_profile_is_open()) {
        /* "is_open" means the profile slot has no peer address, i.e. NOT
         * bonded. Bonded is its negation. */
        flags |= STATUS_ADV_FLAG_BLE_BONDED;
    }
    /* CAPS_WORD and CHARGING intentionally never set -- status_adv_wire.h. */
    payload[STATUS_ADV_OFF_STATUS_FLAGS] = flags;

    /* Upstream's semantics, preserved for wire compatibility even though the
     * scanner drops it as "a strictly worse encoding of usb_hid_ready". */
    payload[STATUS_ADV_OFF_CONNECTION_COUNT] = (uint8_t)(1 + (usb_hid ? 1 : 0));

    payload[STATUS_ADV_OFF_DEVICE_ROLE] =
        IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) ? STATUS_ADV_ROLE_CENTRAL
                                                  : STATUS_ADV_ROLE_STANDALONE;
    payload[STATUS_ADV_OFF_DEVICE_INDEX] = 0;

    sys_put_le32(keyboard_id, &payload[STATUS_ADV_OFF_KEYBOARD_ID]);

    /* Explicit modifiers only -- the ones physically held. Implicit mods
     * synthesised for shifted keycodes are a property of the keystroke in
     * flight, not of what the user is holding, and would make the display
     * flicker on every capital letter. Bit order is ZMK's own (MOD_LCTL=0x01
     * .. MOD_RGUI=0x80), so no translation. */
    payload[STATUS_ADV_OFF_MODIFIER_FLAGS] = zmk_hid_get_explicit_mods();

#if IS_ENABLED(CONFIG_ZMK_WPM)
    {
        int wpm = zmk_wpm_get_state();
        payload[STATUS_ADV_OFF_WPM] = (wpm < 0) ? 0 : (wpm > 255 ? 255 : (uint8_t)wpm);
    }
#else
    /* CONFIG_ZMK_WPM is off on this keyboard and we deliberately do NOT
     * select it: it adds a keycode listener and a periodic timer to the
     * central for a cosmetic field. The byte stays 0 (memset above). */
#endif

    payload[STATUS_ADV_OFF_CHANNEL] = (uint8_t)CONFIG_CHARYBDIS_STATUS_ADV_CHANNEL;
}

/* ------------------------------------------------------------------------ */
/* Advertising set handling                                                 */
/* ------------------------------------------------------------------------ */

/* ZMK's own desired_adv computation, from app/src/ble.c update_advertising(),
 * expressed with the two public predicates it uses. True means "ZMK wants the
 * advertising set"; we then keep our hands off it entirely. */
static bool zmk_wants_advertiser(void) {
    return zmk_ble_active_profile_is_open() || !zmk_ble_active_profile_is_connected();
}

static void release_adv(void) {
    if (!we_own_adv) {
        return;
    }

    int err = bt_le_adv_stop();

    /* Clear the flag regardless of err. If the stop failed we no longer have a
     * coherent claim on the set, and continuing to believe we own it would
     * make us stop somebody else's advertisement later. */
    we_own_adv = false;

    if (err) {
        LOG_WRN("bt_le_adv_stop failed (%d)", err);
    }
}

static void release_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    release_adv();

    /* A burst that was cut short must not leave a PHASE_BURST_END tick pending
     * for a set we no longer hold. */
    phase = PHASE_EMIT;
}

#if IS_ENABLED(CONFIG_CHARYBDIS_STATUS_ADV_PIGGYBACK)
/*
 * Mode 1. ZMK is advertising for pairing; we add our payload to its scan
 * response. We start nothing and stop nothing.
 *
 * OFF BY DEFAULT, AND NOT FOR TIMIDITY: a scan response is only transmitted in
 * answer to a SCAN_REQ, and our display never sends one -- dispscan_observer.c
 * on branch display/scanner uses BT_LE_SCAN_TYPE_PASSIVE deliberately. So as
 * things stand this code would rewrite the keyboard's pairing advertisement
 * and deliver nothing. It is kept, correct and ready, because the fix is one
 * line on the scanner side (passive -> active). See the Kconfig help.
 *
 * -EAGAIN just means ZMK is not actually advertising right now (adv.c returns
 * it when BT_ADV_ENABLED is clear). Common and uninteresting -- for instance
 * between a disconnect and ZMK's deferred update_advertising_work.
 */
static void piggyback(void) {
    int err = bt_le_adv_update_data(zmk_ad_mirror, ARRAY_SIZE(zmk_ad_mirror), beacon_ad,
                                    ARRAY_SIZE(beacon_ad));

    if (err && err != -EAGAIN) {
        LOG_WRN("piggyback update failed (%d)", err);
    }
}
#else
static inline void piggyback(void) {}
#endif

/* ------------------------------------------------------------------------ */
/* Gating                                                                   */
/* ------------------------------------------------------------------------ */

/*
 * SPLIT COLD-BOOT HAZARD.
 *
 * A legacy advertiser transmitting while the central is trying to place an
 * LL_CONNECT_REQ can preempt the 150 us T_IFS response window, and upstream
 * documented exactly that starving split connection at cold boot. This
 * keyboard is a split, so we simply do not transmit while the split link is
 * down -- not at boot, and not after a mid-session drop either, because the
 * radio behaviour is identical in both cases.
 *
 * The hold-off EXPIRES (CONFIG_CHARYBDIS_STATUS_ADV_SPLIT_GRACE_MS, 60 s). A
 * half whose battery is flat leaves the central re-scanning indefinitely, and
 * "display goes permanently dark whenever one half dies" is a worse outcome
 * than "we resume beaconing next to an unsuccessful scan". After the grace
 * window the burst duty cycle is the remaining mitigation.
 *
 * The link signal is the zmk_peripheral_battery_state_changed event, which on
 * the CENTRAL is raised with the real level after the GATT battery read
 * completes and with 0 from split_central_disconnected()
 * (zmk/app/src/split/bluetooth/central.c). Note the deliberate choice NOT to
 * use zmk_split_peripheral_status_changed: at v0.2.1 that event is raised ONLY
 * on the peripheral half (app/src/split/bluetooth/peripheral.c), never on the
 * central, so it is unusable here. Verified, not assumed.
 *
 * KNOWN IMPRECISION: a peripheral genuinely reporting 0% is indistinguishable
 * from a disconnected one, so we would hold off for the grace window and then
 * resume. Harmless, and 0 already means N/A on the wire.
 *
 * SECOND, LESS OBVIOUS BENEFIT. While a split peripheral slot is open the
 * central is SCANNING, and with CONFIG_BT_SCAN_WITH_IDENTITY off an active
 * scan leaves an NRPA in the controller's single shared random-address
 * register. Starting an advertisement then makes bt_id_set_adv_random_addr()
 * actually issue LE_SET_RANDOM_ADDRESS instead of short-circuiting, which the
 * controller may answer Command Disallowed while scanning is enabled. ZMK's
 * own connectable advertisement takes the identical path (adv.c's CONNECTABLE
 * branch calls the same function, for the same stated reason), so this is not
 * a risk we introduce -- but not advertising while the split is down avoids it
 * for us entirely. If it ever does fail, bt_le_adv_start() returns the error,
 * we log and skip the burst; nothing is left half-configured.
 */
static bool split_ok(void) {
    if (STATUS_ADV_EXPECTED_PERIPHERALS == 0) {
        return true;
    }

    /*
     * REFRESH FROM THE PULL BEFORE TRUSTING THE LATCH.
     *
     * `split_connected` used to be fed only by the peripheral-battery event,
     * using state_of_charge == 0 as the "link went down" sentinel. The
     * adversarial review called that signal fragile in both directions, and
     * the on-air capture then showed the event never arriving at all -- which
     * means split_connected sat false from boot, split_ok() fell through to
     * the grace-expiry branch, and the T_IFS hold-off this function exists to
     * provide was silently inoperative. It never failed loudly because
     * "grace expired" and "split is up" produce identical behaviour.
     *
     * zmk_split_get_peripheral_battery_level() returns -ENOTCONN precisely
     * when peripherals[0].state != PERIPHERAL_SLOT_STATE_CONNECTED
     * (central.c:411-413), so it reports the connection state directly rather
     * than inferring it from a battery value that is also a legal reading.
     * A battery of 0 no longer has to mean two different things.
     */
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    {
        uint8_t lvl;
        bool now_up = (zmk_split_get_peripheral_battery_level(0, &lvl) == 0);

        if (now_up != split_connected) {
            split_connected = now_up;
            if (!now_up) {
                split_down_since = k_uptime_get();
            }
        }
    }
#endif

    if (split_connected) {
        return true;
    }
    if (CONFIG_CHARYBDIS_STATUS_ADV_SPLIT_GRACE_MS == 0) {
        return true;
    }
    return (k_uptime_get() - split_down_since) > CONFIG_CHARYBDIS_STATUS_ADV_SPLIT_GRACE_MS;
}

static uint32_t period_ms(void) {
    return (zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE)
               ? CONFIG_CHARYBDIS_STATUS_ADV_ACTIVE_MS
               : CONFIG_CHARYBDIS_STATUS_ADV_IDLE_MS;
}

/* ------------------------------------------------------------------------ */
/* bt_conn_cb -- see mechanism (2) in the header comment                    */
/* ------------------------------------------------------------------------ */

static void status_adv_connected(struct bt_conn *conn, uint8_t err) {
    ARG_UNUSED(conn);
    ARG_UNUSED(err);
    /* Nothing to do. We never hold the set while ZMK is advertising, so a
     * connection forming cannot catch us holding it. */
}

static void status_adv_disconnected(struct bt_conn *conn, uint8_t reason) {
    struct bt_conn_info info;

    ARG_UNUSED(reason);

    if (bt_conn_get_info(conn, &info) != 0) {
        return;
    }

    /* PERIPHERAL role = this was our host link. ZMK's disconnected() is about
     * to submit update_advertising_work to the system workqueue and will want
     * the advertising set. Get out of the way first: we are called before ZMK
     * (LIFO callback list) and we queue to the same FIFO workqueue, so our
     * release is guaranteed to run before its start. */
    if (info.role == BT_CONN_ROLE_PERIPHERAL) {
        if (we_own_adv) {
            k_work_submit(&release_work);
        }
    }
}

static struct bt_conn_cb status_adv_conn_cb = {
    .connected = status_adv_connected,
    .disconnected = status_adv_disconnected,
};

/* ------------------------------------------------------------------------ */
/* Tick                                                                     */
/* ------------------------------------------------------------------------ */

static void tick_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    /*
     * Registered here rather than from SYS_INIT, and the reason is ordering,
     * not laziness. bt_conn_cb_register() prepends, and notify_disconnected()
     * calls the runtime list before the STRUCT_SECTION list, so the callback
     * registered LAST is invoked FIRST. ZMK registers its own from main() ->
     * settings_load() -> the ble/profiles settings commit handler, which is
     * after every SYS_INIT. Registering from the first tick -- several seconds
     * into runtime -- puts us reliably at the head.
     *
     * FRAGILITY, STATED: this is an ordering assumption about somebody else's
     * registration, and Zephyr 3.5 has no bt_conn_cb_unregister() to re-assert
     * it. If ZMK ever re-registers, we lose the head position and mechanism
     * (2) silently degrades to mechanism (3) alone -- the burst duty cycle --
     * which is why (3) exists independently.
     */
    if (!conn_cb_registered) {
        bt_conn_cb_register(&status_adv_conn_cb);
        conn_cb_registered = true;
    }

    if (phase == PHASE_BURST_END) {
        release_adv();
        phase = PHASE_EMIT;
        uint32_t p = period_ms();
        uint32_t rest =
            (p > CONFIG_CHARYBDIS_STATUS_ADV_BURST_MS) ? p - CONFIG_CHARYBDIS_STATUS_ADV_BURST_MS : 1;
        k_work_reschedule(&tick_work, K_MSEC(rest));
        return;
    }

    /* Deep sleep: the radio is about to go away and nothing is changing.
     * Release and idle. ZMK_ACTIVITY_IDLE keeps broadcasting, just slowly. */
    if (zmk_activity_get_state() == ZMK_ACTIVITY_SLEEP || !split_ok()) {
        release_adv();
        k_work_reschedule(&tick_work, K_MSEC(MIN(period_ms(), 5000u)));
        return;
    }

    build_payload();

    if (zmk_wants_advertiser()) {
        /* Mode 1. ZMK owns the set (or is about to). Never contend. */
        release_adv();
        piggyback();
        k_work_reschedule(&tick_work, K_MSEC(period_ms()));
        return;
    }

    /* Mode 2. The active profile is connected, so ZMK's update_advertising()
     * computes desired_adv = ZMK_ADV_NONE and it stopped its advertiser on
     * connect. The set is genuinely free. Take it for one burst. */
    /*
     * CLAIM OWNERSHIP BEFORE STARTING, NOT AFTER. This ordering is the whole
     * fix for a real, verified bug; do not "tidy" it back.
     *
     * bt_le_adv_start() is not atomic and it is not fast. It issues three
     * synchronous HCI commands (SET_ADV_PARAM, SET_ADV_DATA, SET_ADV_ENABLE)
     * and sleeps on each command-complete -- order 1-3 ms, during which the BT
     * RX thread runs. If the host drops the link inside that window and
     * `we_own_adv` is still false, status_adv_disconnected() sees no ownership
     * and does NOT submit release_work. ZMK's disconnected() then submits
     * update_advertising_work (verified: app/src/ble.c:530 defers via
     * k_work_submit, it is not synchronous), which queues BEHIND the tick
     * handler we are currently inside. It runs, hits CHECKED_OPEN_ADV, gets
     * -EALREADY from our beacon, and returns WITHOUT setting
     * advertising_status -- so ZMK believes it is ZMK_ADV_NONE while desiring
     * ZMK_ADV_CONN. Nothing in ble.c polls or retries. The keyboard is then
     * invisible to its host until a profile keypress or a reboot.
     *
     * That failure is worse than the known prof_select papercut, because the
     * user's instinctive response to "my keyboard didn't reconnect" -- type,
     * move the mouse, re-open the lid -- calls update_advertising() on none of
     * those paths.
     *
     * Setting the flag first closes it: a disconnect landing anywhere inside
     * bt_le_adv_start() now sees ownership and submits release_work from the
     * BT RX thread, ahead of ZMK's own submission from the same callback
     * chain, so the FIFO ordering that mechanism (2) depends on is restored.
     *
     * The cost is a transient false-true if the start fails, which we undo
     * immediately below. release_adv() is a no-op against a set we never
     * started (bt_le_adv_stop() on a stopped set returns -EALREADY, which it
     * logs and swallows), so even a release racing in during the failed window
     * is harmless.
     */
    we_own_adv = true;

    int err = bt_le_adv_start(STATUS_ADV_PARAM, beacon_ad, ARRAY_SIZE(beacon_ad), NULL, 0);

    if (err == 0) {
        phase = PHASE_BURST_END;
        k_work_reschedule(&tick_work, K_MSEC(CONFIG_CHARYBDIS_STATUS_ADV_BURST_MS));
        return;
    }

    /* The start failed, so we do not own the set after all. Give the claim
     * back before doing anything else -- in particular before piggyback(),
     * which must never run while we believe we hold somebody else's set. */
    we_own_adv = false;

    if (err == -EALREADY) {
        /* Somebody -- realistically ZMK, racing us between the predicate above
         * and here -- holds the set. It is NOT ours to stop. Fall back to
         * riding whatever they are advertising. */
        LOG_DBG("advertiser busy, falling back to piggyback");
        piggyback();
    } else {
        LOG_WRN("bt_le_adv_start failed (%d)", err);
    }

    k_work_reschedule(&tick_work, K_MSEC(period_ms()));
}

/* ------------------------------------------------------------------------ */
/* Events                                                                   */
/* ------------------------------------------------------------------------ */

/*
 * We subscribe to THREE events, and no more. Everything else on the wire is
 * sampled in build_payload() at tick time, which is already 1 Hz while the
 * keyboard is in use. Subscribing to layer/modifier/keycode changes would add
 * listener work on the central for latency we already have.
 */
static int status_adv_event_listener(const zmk_event_t *eh) {
    if (as_zmk_activity_state_changed(eh) != NULL) {
        /* An idle->active transition must not wait out the 30 s idle period. */
        k_work_reschedule(&tick_work, K_MSEC(50));
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (as_zmk_ble_active_profile_changed(eh) != NULL) {
        /*
         * A profile switch is the one transition we cannot pre-empt: ZMK's
         * zmk_ble_prof_select() calls update_advertising() synchronously and
         * only raises this event afterwards. So by the time we see it, the
         * -EALREADY (if any) has already happened.
         *
         * What we can still do is get out of the way and STAY out of the way
         * for a beat, so that the user's inevitable second press finds the set
         * free. Release now; do not emit for one full active period.
         */
        if (we_own_adv) {
            k_work_submit(&release_work);
        }
        k_work_reschedule(&tick_work, K_MSEC(CONFIG_CHARYBDIS_STATUS_ADV_ACTIVE_MS));
        return ZMK_EV_EVENT_BUBBLE;
    }

/*
 * GATE ON THE SYMBOL THAT ACTUALLY RAISES THE EVENT, and keep this expression
 * character-identical to the one on the ZMK_SUBSCRIPTION below.
 *
 * zmk_peripheral_battery_state_changed is emitted from
 * app/src/split/bluetooth/central.c under
 * `#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)` --
 * both the periodic GATT read and, at central.c:979-984, the
 * state_of_charge = 0 raised on split disconnect that split_ok() depends on.
 * It is NOT gated on ZMK_BATTERY_REPORTING && ZMK_SPLIT_ROLE_CENTRAL, which is
 * what this used to test.
 *
 * WHY THE OLD GUARD WAS DANGEROUS RATHER THAN MERELY WRONG: those two symbol
 * sets can diverge (battery reporting on, peripheral fetching off). In that
 * configuration we subscribed to an event nothing ever raises, so
 * `split_connected` stayed false forever, so after CHARYBDIS_STATUS_ADV_
 * SPLIT_GRACE_MS the split hold-off -- the T_IFS cold-boot protection this
 * module's whole safety story rests on -- was permanently disabled, with no
 * symptom and no log line. Failing closed here is not optional.
 */
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    const struct zmk_peripheral_battery_state_changed *pb =
        as_zmk_peripheral_battery_state_changed(eh);
    if (pb != NULL) {
        if (pb->source == 0) {
            /* Only slot 0 reaches the wire (offset 12); [1]/[2] are aux slots
             * this keyboard does not have. */
            peripheral_batt = pb->state_of_charge;
        }

        /* Doubles as the split link signal -- see split_ok(). */
        bool now_connected = (pb->state_of_charge != 0);
        if (now_connected != split_connected) {
            split_connected = now_connected;
            if (!now_connected) {
                split_down_since = k_uptime_get();
                LOG_DBG("split link down, holding off");
            } else {
                LOG_DBG("split link up");
            }
        }
        return ZMK_EV_EVENT_BUBBLE;
    }
#endif

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(charybdis_status_adv, status_adv_event_listener);
ZMK_SUBSCRIPTION(charybdis_status_adv, zmk_activity_state_changed);
ZMK_SUBSCRIPTION(charybdis_status_adv, zmk_ble_active_profile_changed);
/* Must stay character-identical to the guard on the `pb` handler above -- see
 * the long note there. Divergence silently disables the split hold-off. */
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
ZMK_SUBSCRIPTION(charybdis_status_adv, zmk_peripheral_battery_state_changed);
#endif

/* ------------------------------------------------------------------------ */
/* Init                                                                     */
/* ------------------------------------------------------------------------ */

static int status_adv_init(void) {
    compute_keyboard_id();

    /* Start assuming the split is down, so the initial delay and the split
     * hold-off compose: nothing goes on air until the peripheral has actually
     * reported in (or the grace window expires). */
    split_connected = false;
    split_down_since = k_uptime_get();

    k_work_schedule(&tick_work, K_MSEC(CONFIG_CHARYBDIS_STATUS_ADV_INITIAL_DELAY_MS));

    return 0;
}

/*
 * APPLICATION, after ZMK's BLE init (CONFIG_ZMK_BLE_INIT_PRIORITY, default 50).
 * Nothing here touches the radio -- it only reads hwinfo and arms a delayed
 * work item -- so the priority is about being tidy, not about correctness.
 */
SYS_INIT(status_adv_init, APPLICATION, 99);
