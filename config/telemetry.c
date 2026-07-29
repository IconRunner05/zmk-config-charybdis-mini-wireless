/*
 * Charybdis USB telemetry + control (diagnostic instrumentation).
 *
 * A Zephyr shell subcommand set ("charybdis ...") on the RIGHT half's USB CDC
 * lets a host TUI (scripts/charybdis_dashboard.py) poll live health and toggle
 * the trackball power rail. The zmk-usb-logging snippet already points
 * zephyr,shell-uart at that CDC ACM node, so enabling CONFIG_SHELL gives a
 * command prompt with no extra device.
 *
 *   charybdis telem                 print one machine-parseable line:
 *       TELEM up=<sec> batL=<pct> batR=<pct> vR=<mV> cpu=<pct>
 *         up   = uptime, seconds since boot
 *         batL = LEFT  (peripheral) battery %  (-1 until the first split report)
 *         batR = RIGHT (central)    battery %
 *         vR   = RIGHT battery millivolts      (-1 if the sensor read fails)
 *         cpu  = CPU busy % since the previous `charybdis telem` call
 *                (-1 on the first call — busy% needs two runtime-stats samples)
 *   charybdis power on|off|toggle   switch the EXT_POWER rail (trackball +
 *                                   peripheral VCC) without unplugging
 *   charybdis ver                   print VER git=<build desc>
 *
 * WHY POLLED, NOT LOGGED. When CONFIG_SHELL owns the USB CDC there is no active
 * log backend on it (a bare LOG_INF goes nowhere), so telemetry is emitted with
 * shell_print from the command handler — which runs on the shell thread, so the
 * output is race-free. The host drives the cadence by re-issuing `charybdis
 * telem` (see the dashboard's poll loop).
 *
 * Hardware limits (why the line is shaped this way):
 *   - nice!nano_v2 has NO fuel-gauge IC -> no current sensing. vR is voltage
 *     only; instantaneous draw is unmeasurable. Drain RATE is estimated
 *     host-side from the vR/batR slope over the session (see the dashboard).
 *   - The split protocol proxies the LEFT half's SoC (%) but not its voltage,
 *     so there is no vL -- only batL.
 *
 * DIAGNOSTIC BUILD ONLY -- gated by CONFIG_CHARYBDIS_TELEMETRY (default n),
 * enabled on the charybdis_right_telem artifact (build.yaml) and the local
 * `make telem` target. The normal daily-driver right build has neither the USB
 * console nor the shell, so none of this compiles into it.
 */

#ifdef CONFIG_CHARYBDIS_TELEMETRY

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/shell/shell.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <drivers/ext_power.h>

#include <zmk/battery.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>

/* Build fingerprint: defined for the whole config zephyr_library by
 * config/CMakeLists.txt (zephyr_library_compile_definitions). Fallback keeps
 * this translation unit independently compilable. */
#ifndef CHARYBDIS_GIT_DESC
#define CHARYBDIS_GIT_DESC "nogit"
#endif

/* LEFT (peripheral) battery %, latched from split battery reports. -1 = not yet
 * heard from the peripheral (no report since boot / peripheral disconnected). */
static int s_bat_left = -1;

/* CPU busy% is a delta metric: it needs the cycle counters from the PREVIOUS
 * `charybdis telem`. The first call reports -1 until this baseline exists. */
static uint64_t s_prev_total; /* non-idle cycles at last sample */
static uint64_t s_prev_exec;  /* idle+non-idle cycles at last sample */
static bool s_have_prev;

/* The RIGHT-half battery voltage sensor: the shield's `vbatt` node
 * (compatible zmk,battery-nrf-vddh). Fetched directly by node label so this
 * does not depend on whether `chosen zmk,battery` is set. */
static const struct device *vbatt_dev(void)
{
	static const struct device *dev;

	if (dev == NULL) {
		dev = DEVICE_DT_GET(DT_NODELABEL(vbatt));
	}
	return dev;
}

/* RIGHT battery millivolts, or -1 if the sensor is not ready / read fails. */
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
	/* sensor_value is volts: val1 = whole volts, val2 = microvolts. */
	return (v.val1 * 1000) + (v.val2 / 1000);
}

/* Whole-CPU busy percentage over the window since the previous call.
 *
 * With CONFIG_SCHED_THREAD_USAGE the per-CPU stats give:
 *   execution_cycles = idle + non-idle cycles (i.e. total elapsed)
 *   total_cycles     = non-idle cycles only
 * so busy% = 100 * d(non-idle) / d(total). Returns -1 on the first call (no
 * baseline yet) or if the stats API is unavailable. */
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

static void telem_format(char *buf, size_t n)
{
	uint32_t up_s = (uint32_t)(k_uptime_get() / 1000);
	int bat_r = zmk_battery_state_of_charge();

	snprintf(buf, n, "TELEM up=%u batL=%d batR=%d vR=%d cpu=%d",
		 up_s, s_bat_left, bat_r, right_millivolts(), cpu_busy_pct());
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

/* --- shell: `charybdis telem | power on|off|toggle | ver` ------------------ */
static int cmd_telem(const struct shell *sh, size_t argc, char **argv)
{
	char line[96];

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	telem_format(line, sizeof(line));
	shell_print(sh, "%s", line);
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
	/* Stable, greppable ack so the dashboard can confirm the new state. */
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
	SHELL_CMD_ARG(power, NULL, "EXT_POWER rail: on|off|toggle", cmd_power, 2, 0),
	SHELL_CMD(ver, NULL, "print firmware build desc", cmd_ver),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(charybdis, &sub_charybdis, "Charybdis diagnostics/control", NULL);

#endif /* CONFIG_CHARYBDIS_TELEMETRY */
