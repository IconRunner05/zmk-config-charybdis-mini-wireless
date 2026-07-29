/*
 * Charybdis USB telemetry + control (diagnostic instrumentation).
 *
 * Emits a periodic, machine-parseable TELEM line over the USB CDC console so a
 * host TUI (scripts/charybdis_dashboard.py) can render live health at a glance:
 *
 *   TELEM up=8231 batL=82 batR=76 vR=3810 cpu=3
 *     up   = uptime, seconds since boot
 *     batL = LEFT  (peripheral) battery %   (-1 until the first split report)
 *     batR = RIGHT (central)    battery %
 *     vR   = RIGHT battery millivolts       (-1 if the sensor read fails)
 *     cpu  = CPU busy % over the interval since the previous TELEM line
 *            (-1 on the first line — a busy% needs two runtime-stats samples)
 *
 * Two-way control. A Zephyr shell subcommand set ("charybdis ...") rides the
 * SAME USB CDC: the zmk-usb-logging snippet already points BOTH zephyr,console
 * and zephyr,shell-uart at that CDC ACM node, so enabling CONFIG_SHELL gives a
 * command prompt with no extra device. Logs auto-route through the shell log
 * backend, so TELEM lines and the prompt coexist on one port.
 *
 *   charybdis power on|off|toggle   toggle the EXT_POWER rail (trackball +
 *                                   peripheral VCC) without unplugging
 *   charybdis telem                 force an immediate TELEM line
 *
 * Hardware limits (why the line is shaped this way):
 *   - nice!nano_v2 has NO fuel-gauge IC -> no current sensing. vR is voltage
 *     only; instantaneous draw is unmeasurable. Drain RATE is estimated
 *     host-side from the vR/batR slope over the session (see the dashboard).
 *   - The split protocol proxies the LEFT half's SoC (%) but not its voltage,
 *     so there is no vL — only batL.
 *
 * DIAGNOSTIC BUILD ONLY -- gated by CONFIG_CHARYBDIS_TELEMETRY (default n),
 * enabled on the charybdis_right_telem artifact (build.yaml) and the local
 * `make telem` target. The normal daily-driver right build has neither the USB
 * console nor the shell, so none of this compiles into it.
 */

#ifdef CONFIG_CHARYBDIS_TELEMETRY

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <errno.h>
#include <string.h>

#include <drivers/ext_power.h>

#include <zmk/battery.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>

LOG_MODULE_REGISTER(charybdis_telem, LOG_LEVEL_INF);

/* LEFT (peripheral) battery %, latched from split battery reports. -1 = not yet
 * heard from the peripheral (no report since boot / peripheral disconnected). */
static int s_bat_left = -1;

/* CPU busy% is a delta metric: it needs the cycle counters from the PREVIOUS
 * sample. First emit reports -1 until this baseline exists. */
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

static void telem_emit(void)
{
	uint32_t up_s = (uint32_t)(k_uptime_get() / 1000);
	int bat_r = zmk_battery_state_of_charge();

	LOG_INF("TELEM up=%u batL=%d batR=%d vR=%d cpu=%d",
		up_s, s_bat_left, bat_r, right_millivolts(), cpu_busy_pct());
}

static void telem_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(telem_work, telem_work_fn);

static void telem_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	telem_emit();
	k_work_reschedule(&telem_work,
			  K_MSEC(CONFIG_CHARYBDIS_TELEMETRY_INTERVAL_MS));
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

/* --- shell: `charybdis power on|off|toggle` / `charybdis telem` ------------- */
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

static int cmd_telem(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(sh);
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	telem_emit();
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_charybdis,
	SHELL_CMD_ARG(power, NULL, "EXT_POWER rail: on|off|toggle", cmd_power, 2, 0),
	SHELL_CMD(telem, NULL, "emit a TELEM line now", cmd_telem),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(charybdis, &sub_charybdis, "Charybdis diagnostics/control", NULL);

/* --- start the periodic emitter -------------------------------------------- */
static int telem_init(void)
{
	/* First emit one interval in, by which point the battery sensor,
	 * ext_power, and the split link have all come up. */
	k_work_schedule(&telem_work,
			K_MSEC(CONFIG_CHARYBDIS_TELEMETRY_INTERVAL_MS));
	return 0;
}
SYS_INIT(telem_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif /* CONFIG_CHARYBDIS_TELEMETRY */
