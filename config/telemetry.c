/*
 * Charybdis USB telemetry + control (diagnostic instrumentation).
 *
 * A Zephyr shell subcommand set ("charybdis ...") on the RIGHT half's USB CDC
 * lets a host TUI (scripts/charybdis_dashboard.py) poll live health and toggle
 * the trackball power rail. The zmk-usb-logging snippet already points
 * zephyr,shell-uart at that CDC ACM node, so enabling CONFIG_SHELL gives a
 * command prompt with no extra device.
 *
 *   charybdis telem     one machine-parseable health line (see telem_format):
 *       up    uptime seconds                 out   selected endpoint usb|ble|none
 *       batL  LEFT (periph) battery %        prof  active BLE profile index
 *       batR  RIGHT (central) battery %      pconn active profile connected 0|1
 *       vR    RIGHT battery millivolts       act   activity A(ctive)|I(dle)|S(leep)
 *       cpu   CPU busy % since last call     temp  die temp, deci-°C (-9990 n/a)
 *       rst   last reset cause               rssi  host-link RSSI dBm (127 n/a)
 *       periph split peripheral links up     ci/lat/sto  host conn interval(1.25ms)/
 *       tbr   trackball events since last          latency/supervision-timeout(10ms)
 *   charybdis threads   one THREAD line per thread: name, free/size stack bytes,
 *                       cumulative CPU cycles (host diffs cyc for per-thread CPU%)
 *   charybdis power on|off|toggle   switch the EXT_POWER (trackball) rail
 *   charybdis ver       VER git=<build desc>
 *
 * WHY POLLED, NOT LOGGED. When CONFIG_SHELL owns the USB CDC there is no active
 * log backend on it (a bare LOG_INF goes nowhere), so telemetry is emitted with
 * shell_print from the command handler — which runs on the shell thread, so the
 * output is race-free. The host drives the cadence by re-issuing the command.
 *
 * Hardware limits: nice!nano_v2 has NO fuel gauge -> no current sensing (vR is
 * voltage only; drain rate is estimated host-side). The split link proxies the
 * LEFT half's SoC (%) but not its voltage, so there is no vL.
 *
 * DIAGNOSTIC BUILD ONLY -- gated by CONFIG_CHARYBDIS_TELEMETRY (default n),
 * enabled on the charybdis_right_telem artifact (build.yaml) and `make telem`.
 * The normal daily-driver right build has neither the USB console nor the shell.
 */

#ifdef CONFIG_CHARYBDIS_TELEMETRY

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/shell/shell.h>
#include <zephyr/input/input.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/hci_types.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <drivers/ext_power.h>

#include <zmk/battery.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/activity.h>

#ifndef CHARYBDIS_GIT_DESC
#define CHARYBDIS_GIT_DESC "nogit"
#endif

#define TEMP_NA (-9990) /* die temp sentinel: driver absent / read failed */
#define RSSI_NA (127)   /* RSSI sentinel: no host link / read failed */

/* LEFT (peripheral) battery %, latched from split battery reports. -1 = unheard. */
static int s_bat_left = -1;

/* CPU busy% is a delta metric needing the previous sample's cycle counters. */
static uint64_t s_prev_total;
static uint64_t s_prev_exec;
static bool s_have_prev;

/* Trackball input-event counter, bumped from the input callback; the TELEM line
 * reports the delta since the previous line (a rough report rate). */
static atomic_t s_tb_count;
static uint32_t s_tb_prev;

/* --- battery voltage (RIGHT, local vbatt sensor) --------------------------- */
static const struct device *vbatt_dev(void)
{
	static const struct device *dev;

	if (dev == NULL) {
		dev = DEVICE_DT_GET(DT_NODELABEL(vbatt));
	}
	return dev;
}

static int right_millivolts(void)
{
	const struct device *dev = vbatt_dev();
	struct sensor_value v;

	if (!device_is_ready(dev)) {
		return -1;
	}
	if (sensor_sample_fetch(dev) < 0) {
		return -1;
	}
	if (sensor_channel_get(dev, SENSOR_CHAN_GAUGE_VOLTAGE, &v) < 0) {
		return -1;
	}
	return (v.val1 * 1000) + (v.val2 / 1000);
}

/* --- die temperature (best-effort; -9990 if the driver is not present) ------ */
#if DT_NODE_HAS_STATUS(DT_NODELABEL(temp), okay)
static int die_temp_dc(void) /* deci-Celsius */
{
	const struct device *t = DEVICE_DT_GET(DT_NODELABEL(temp));
	struct sensor_value v;

	if (!device_is_ready(t)) {
		return TEMP_NA;
	}
	if (sensor_sample_fetch(t) < 0) {
		return TEMP_NA;
	}
	if (sensor_channel_get(t, SENSOR_CHAN_DIE_TEMP, &v) < 0) {
		return TEMP_NA;
	}
	return (v.val1 * 10) + (v.val2 / 100000);
}
#else
static int die_temp_dc(void)
{
	return TEMP_NA;
}
#endif

/* --- CPU busy% over the window since the previous call ---------------------- */
static int cpu_busy_pct(void)
{
	k_thread_runtime_stats_t s;
	int pct = -1;

	if (k_thread_runtime_stats_cpu_get(0, &s) < 0) {
		return -1;
	}
	if (s_have_prev) {
		uint64_t d_exec = s.execution_cycles - s_prev_exec;
		uint64_t d_busy = s.total_cycles - s_prev_total;

		if (d_exec > 0) {
			pct = (int)((d_busy * 100U) / d_exec);
		}
	}
	s_prev_exec = s.execution_cycles;
	s_prev_total = s.total_cycles;
	s_have_prev = true;
	return pct;
}

/* --- last reset cause (read once, latched) ---------------------------------- */
static const char *reset_str(void)
{
	static bool done;
	static uint32_t cause;

	if (!done) {
		if (hwinfo_get_reset_cause(&cause) != 0) {
			cause = 0;
		}
		done = true;
	}
	if (cause & RESET_WATCHDOG) {
		return "WDT";
	}
	if (cause & RESET_CPU_LOCKUP) {
		return "LOCKUP";
	}
	if (cause & RESET_BROWNOUT) {
		return "BOR";
	}
	if (cause & RESET_SOFTWARE) {
		return "SOFT";
	}
	if (cause & RESET_PIN) {
		return "PIN";
	}
	if (cause & RESET_POR) {
		return "POR";
	}
	if (cause == 0) {
		return "none";
	}
	return "other";
}

/* --- split peripheral link count (LE connections where WE are central) ------ */
static void periph_count_cb(struct bt_conn *conn, void *ud)
{
	struct bt_conn_info info;

	if (bt_conn_get_info(conn, &info) != 0) {
		return;
	}
	if (info.type == BT_CONN_TYPE_LE && info.role == BT_CONN_ROLE_CENTRAL) {
		(*(int *)ud)++;
	}
}

static int periph_links(void)
{
	int n = 0;

	bt_conn_foreach(BT_CONN_TYPE_LE, periph_count_cb, &n);
	return n;
}

/* --- host link: conn params + RSSI (the profile we present HID to) ---------- */
static void host_link(int *ci, int *lat, int *sto, int *rssi)
{
	struct bt_conn *conn = zmk_ble_active_profile_conn(); /* refs; unref below */
	struct bt_conn_info info;
	uint16_t handle;

	*ci = 0;
	*lat = 0;
	*sto = 0;
	*rssi = RSSI_NA;

	if (conn == NULL) {
		return;
	}
	if (bt_conn_get_info(conn, &info) == 0 && info.type == BT_CONN_TYPE_LE) {
		*ci = info.le.interval;
		*lat = info.le.latency;
		*sto = info.le.timeout;
	}
	if (bt_hci_get_conn_handle(conn, &handle) == 0) {
		struct net_buf *buf = bt_hci_cmd_create(
			BT_HCI_OP_READ_RSSI, sizeof(struct bt_hci_cp_read_rssi));

		if (buf != NULL) {
			struct bt_hci_cp_read_rssi *cp =
				net_buf_add(buf, sizeof(*cp));
			struct net_buf *rsp = NULL;

			cp->handle = sys_cpu_to_le16(handle);
			if (bt_hci_cmd_send_sync(BT_HCI_OP_READ_RSSI, buf, &rsp) == 0 &&
			    rsp != NULL) {
				struct bt_hci_rp_read_rssi *rp =
					(void *)rsp->data;

				*rssi = rp->rssi;
				net_buf_unref(rsp);
			}
		}
	}
	bt_conn_unref(conn);
}

static void telem_format(char *buf, size_t n)
{
	uint32_t up_s = (uint32_t)(k_uptime_get() / 1000);
	int bat_r = zmk_battery_state_of_charge();
	struct zmk_endpoint_instance ep = zmk_endpoint_get_selected();
	const char *out = (ep.transport == ZMK_TRANSPORT_USB)   ? "usb"
			  : (ep.transport == ZMK_TRANSPORT_BLE) ? "ble"
								: "none";
	enum zmk_activity_state a = zmk_activity_get_state();
	char act = (a == ZMK_ACTIVITY_ACTIVE) ? 'A' : (a == ZMK_ACTIVITY_IDLE) ? 'I' : 'S';
	int ci, lat, sto, rssi;
	uint32_t tb_now = (uint32_t)atomic_get(&s_tb_count);
	uint32_t tbr = tb_now - s_tb_prev;

	s_tb_prev = tb_now;
	host_link(&ci, &lat, &sto, &rssi);

	snprintf(buf, n,
		 "TELEM up=%u batL=%d batR=%d vR=%d cpu=%d rst=%s periph=%d "
		 "out=%s prof=%d pconn=%d act=%c temp=%d rssi=%d ci=%d lat=%d "
		 "sto=%d tbr=%u",
		 up_s, s_bat_left, bat_r, right_millivolts(), cpu_busy_pct(),
		 reset_str(), periph_links(), out, zmk_ble_active_profile_index(),
		 zmk_ble_active_profile_is_connected() ? 1 : 0, act, die_temp_dc(),
		 rssi, ci, lat, sto, tbr);
}

/* --- per-thread snapshot (collected under k_thread_foreach, printed after) --- */
#define THREADS_MAX 40
struct thread_row {
	char name[24];
	size_t free;
	size_t size;
	uint64_t cyc;
};
static struct thread_row s_rows[THREADS_MAX];
static int s_row_n;

static void thread_row_cb(const struct k_thread *thr, void *ud)
{
	struct k_thread *t = (struct k_thread *)thr;
	size_t unused = 0;
	k_thread_runtime_stats_t st;
	const char *nm;

	ARG_UNUSED(ud);
	if (s_row_n >= THREADS_MAX) {
		return;
	}
	struct thread_row *r = &s_rows[s_row_n++];

	nm = k_thread_name_get(t);
	if (nm == NULL || nm[0] == '\0') {
		nm = "?";
	}
	strncpy(r->name, nm, sizeof(r->name) - 1);
	r->name[sizeof(r->name) - 1] = '\0';

	r->free = (k_thread_stack_space_get(t, &unused) == 0) ? unused : 0;
	r->size = t->stack_info.size;
	r->cyc = (k_thread_runtime_stats_get(t, &st) == 0) ? st.execution_cycles : 0;
}

/* --- battery event listener: latch the LEFT (peripheral) SoC --------------- */
static int telem_listener(const zmk_event_t *eh)
{
	const struct zmk_peripheral_battery_state_changed *p =
		as_zmk_peripheral_battery_state_changed(eh);

	if (p != NULL) {
		s_bat_left = p->state_of_charge;
	}
	return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(charybdis_telem, telem_listener);
ZMK_SUBSCRIPTION(charybdis_telem, zmk_peripheral_battery_state_changed);

/* --- trackball input counter ----------------------------------------------- */
static void tb_input_cb(struct input_event *evt, void *ud)
{
	ARG_UNUSED(evt);
	ARG_UNUSED(ud);
	atomic_inc(&s_tb_count);
}
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_NODELABEL(trackball)), tb_input_cb, NULL);

/* --- shell commands -------------------------------------------------------- */
static int cmd_telem(const struct shell *sh, size_t argc, char **argv)
{
	char line[256];

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	telem_format(line, sizeof(line));
	shell_print(sh, "%s", line);
	return 0;
}

static int cmd_threads(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	/* Collect under the foreach lock (no blocking calls in the cb), then
	 * print outside it so shell_print can never deadlock the scheduler. */
	s_row_n = 0;
	k_thread_foreach(thread_row_cb, NULL);
	for (int i = 0; i < s_row_n; i++) {
		shell_print(sh, "THREAD name=%s free=%u size=%u cyc=%llu",
			    s_rows[i].name, (unsigned)s_rows[i].free,
			    (unsigned)s_rows[i].size,
			    (unsigned long long)s_rows[i].cyc);
	}
	shell_print(sh, "THREADEND n=%d", s_row_n);
	return 0;
}

static int cmd_power(const struct shell *sh, size_t argc, char **argv)
{
	const struct device *ep = device_get_binding("EXT_POWER");
	const char *arg = argv[1];
	int rc;

	ARG_UNUSED(argc);

	if (ep == NULL) {
		shell_error(sh, "EXT_POWER device not found");
		return -ENODEV;
	}

	if (strcmp(arg, "on") == 0) {
		rc = ext_power_enable(ep);
	} else if (strcmp(arg, "off") == 0) {
		rc = ext_power_disable(ep);
	} else if (strcmp(arg, "toggle") == 0) {
		rc = (ext_power_get(ep) > 0) ? ext_power_disable(ep)
					     : ext_power_enable(ep);
	} else {
		shell_error(sh, "usage: charybdis power on|off|toggle");
		return -EINVAL;
	}

	if (rc < 0) {
		shell_error(sh, "ext_power op failed (%d)", rc);
		return rc;
	}
	shell_print(sh, "EXTPOWER state=%d", ext_power_get(ep));
	return 0;
}

static int cmd_ver(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(sh, "VER git=%s", CHARYBDIS_GIT_DESC);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_charybdis,
	SHELL_CMD(telem, NULL, "print one TELEM health line", cmd_telem),
	SHELL_CMD(threads, NULL, "print per-thread stack + CPU cycles", cmd_threads),
	SHELL_CMD_ARG(power, NULL, "EXT_POWER rail: on|off|toggle", cmd_power, 2, 0),
	SHELL_CMD(ver, NULL, "print firmware build desc", cmd_ver),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(charybdis, &sub_charybdis, "Charybdis diagnostics/control", NULL);

#endif /* CONFIG_CHARYBDIS_TELEMETRY */
