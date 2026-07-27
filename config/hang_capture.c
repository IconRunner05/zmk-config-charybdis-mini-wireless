/*
 * Watchdog hang capture + boot replay (debug instrumentation).
 *
 * The failure we are hunting is a HANG on the central half: the CPU stops
 * scheduling, no fault handler runs, and a deferred USB log never drains — so a
 * plain serial capture shows nothing at the moment of death. This module makes
 * the hang self-report:
 *
 *   1. A low-priority heartbeat thread feeds the hardware watchdog while the
 *      scheduler is healthy. A hang starves that thread -> the feed stops.
 *   2. On watchdog timeout an ISR fires ~2 LFCLK cycles (~61us) before the SoC
 *      resets — enough for a few RAM writes (NOT flash). It records a minimal
 *      fingerprint of the thread that was running when the feed stopped into a
 *      __noinit struct that survives the (warm) watchdog reset.
 *   3. At the next boot this module logs that fingerprint as a HANGDUMP line
 *      over USB. The serial-capture script's reconnect loop stitches the reboot
 *      and the replayed line into the same logfile.
 *
 * Watchdog is armed with WDT_OPT_PAUSE_IN_SLEEP so it cannot fire during
 * legitimate deep sleep (CONFIG_ZMK_SLEEP) — it only counts while the CPU is
 * awake/spinning, which is exactly the activity-gated hang we are chasing.
 *
 * Bonus: the watchdog auto-recovers the half (no manual power-cycle), and the
 * reset cause (RESET_WATCHDOG) distinguishes a hang from any other reset even
 * when the ISR could not run (e.g. an interrupts-disabled IRQ storm).
 *
 * DEBUG BUILD ONLY — gated by CONFIG_CHARYBDIS_HANG_CAPTURE (default n).
 */

#ifdef CONFIG_CHARYBDIS_HANG_CAPTURE

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/drivers/hwinfo.h>
#include <cmsis_core.h>

LOG_MODULE_REGISTER(hang_capture, LOG_LEVEL_INF);

#define WDT_NODE DT_NODELABEL(wdt0)

#define HANG_MAGIC 0x48414E47u /* "HANG" */

/* Fingerprint of the moment the heartbeat stopped. __noinit keeps it out of the
 * zeroed .bss so it survives a warm watchdog reset (nRF52 retains RAM across a
 * WDT reset). */
struct hang_record {
	uint32_t magic;      /* HANG_MAGIC once every field below is written */
	uint32_t uptime_ms;  /* device uptime at the timeout */
	uint32_t pc;         /* best-effort interrupted PC */
	uint32_t lr;         /* best-effort interrupted LR */
	char thread[24];     /* name of the thread that was running */
};

static __noinit struct hang_record hang_rec;

static const struct device *const wdt = DEVICE_DT_GET(WDT_NODE);
static int wdt_channel = -1;
static volatile bool wdt_armed;

/* Latched copy of a prior-boot capture, re-emitted several times a few seconds
 * into boot. The one-shot init log (~0.36s) fires while the USB CDC console is
 * still re-enumerating after the warm reset and is routinely dropped (observed:
 * BUILDSTAMP + "armed" + HANGDUMP all lost in the 0.33-5s window while the host
 * re-attached). Re-emitting past the feed interval, once USB is stable, reliably
 * lands the line in the serial capture. */
#define HANG_DUMP_REPLAYS 6
static bool s_dump_pending;
static bool s_dump_norecord; /* watchdog reset but ISR left no record */
static uint32_t s_dump_uptime, s_dump_pc, s_dump_lr;
static char s_dump_thread[sizeof(((struct hang_record *)0)->thread)];

static void hang_dump_emit(unsigned int rep)
{
	if (s_dump_norecord) {
		LOG_INF("HANGDUMP(%u/%u) watchdog reset, no captured record "
			"(ISR did not run — likely IRQ storm / IRQs disabled)",
			rep, HANG_DUMP_REPLAYS);
	} else {
		LOG_INF("HANGDUMP(%u/%u) prev-hang uptime=%ums thread='%s' "
			"pc=0x%08x lr=0x%08x",
			rep, HANG_DUMP_REPLAYS, s_dump_uptime,
			s_dump_thread[0] != '\0' ? s_dump_thread : "?",
			s_dump_pc, s_dump_lr);
	}
}

/* WDT timeout ISR. Tiny window before reset — RAM writes only. */
static void wdt_timeout_cb(const struct device *dev, int channel_id)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(channel_id);

	hang_rec.uptime_ms = k_uptime_get_32();

	/* Interrupted thread's pre-exception context is stacked on PSP:
	 * r0,r1,r2,r3,r12,LR,PC,xPSR. We are in the WDT ISR on MSP, so PSP still
	 * points at that frame. Best-effort (zero if PSP looks unusable). */
	uint32_t psp = __get_PSP();

	if (psp) {
		const uint32_t *frame = (const uint32_t *)psp;

		hang_rec.lr = frame[5];
		hang_rec.pc = frame[6];
	} else {
		hang_rec.lr = 0;
		hang_rec.pc = 0;
	}

	const char *name = k_thread_name_get(k_current_get());

	hang_rec.thread[0] = '\0';
	if (name != NULL) {
		size_t i = 0;

		for (; name[i] != '\0' && i < sizeof(hang_rec.thread) - 1; i++) {
			hang_rec.thread[i] = name[i];
		}
		hang_rec.thread[i] = '\0';
	}

	hang_rec.magic = HANG_MAGIC; /* set last: record is valid only when complete */
	/* return -> SoC reset (WDT_FLAG_RESET_SOC) */
}

/* Low-priority heartbeat: only starved (for the full timeout) by a real hang. */
static void hang_heartbeat(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (!wdt_armed) {
		k_sleep(K_MSEC(100));
	}

	unsigned int dump_reps = 0;

	for (;;) {
		(void)wdt_feed(wdt, wdt_channel);

		/* Re-emit the boot replay a few times once USB is stable. The
		 * first pass (~0.4s) is still lost to re-enumeration; passes at
		 * ~feed intervals thereafter land. */
		if (s_dump_pending && dump_reps < HANG_DUMP_REPLAYS) {
			hang_dump_emit(++dump_reps);
		}

		k_sleep(K_MSEC(CONFIG_CHARYBDIS_HANG_CAPTURE_FEED_MS));
	}
}

/* 768 (was 512): the periodic HANGDUMP re-emit adds a LOG_INF (with args) to
 * this thread's deepest path; 512 left only ~128 bytes headroom. */
K_THREAD_DEFINE(hang_hb_tid, 768, hang_heartbeat, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

static int hang_capture_init(void)
{
	/* 1. Latch a capture from a prior watchdog reset (warm reset kept RAM).
	 *    The heartbeat thread re-emits it a few seconds in; logging it here
	 *    (~0.36s) races USB re-enumeration and is routinely dropped. */
	if (hang_rec.magic == HANG_MAGIC) {
		s_dump_uptime = hang_rec.uptime_ms;
		s_dump_pc = hang_rec.pc;
		s_dump_lr = hang_rec.lr;

		size_t i = 0;

		for (; hang_rec.thread[i] != '\0' && i < sizeof(s_dump_thread) - 1; i++) {
			s_dump_thread[i] = hang_rec.thread[i];
		}
		s_dump_thread[i] = '\0';
		s_dump_pending = true;
		hang_rec.magic = 0;
	} else {
		uint32_t cause = 0;

		if (hwinfo_get_reset_cause(&cause) == 0 && (cause & RESET_WATCHDOG)) {
			s_dump_norecord = true;
			s_dump_pending = true;
		}
		(void)hwinfo_clear_reset_cause();
	}

	/* 2. Arm the watchdog. Failures log but never block boot. */
	if (!device_is_ready(wdt)) {
		LOG_ERR("hang_capture: wdt device not ready");
		return 0;
	}

	struct wdt_timeout_cfg cfg = {
		.window = { .min = 0U, .max = CONFIG_CHARYBDIS_HANG_CAPTURE_TIMEOUT_MS },
		.callback = wdt_timeout_cb,
		.flags = WDT_FLAG_RESET_SOC,
	};

	wdt_channel = wdt_install_timeout(wdt, &cfg);
	if (wdt_channel < 0) {
		LOG_ERR("hang_capture: wdt_install_timeout failed (%d)", wdt_channel);
		return 0;
	}

	int rc = wdt_setup(wdt, WDT_OPT_PAUSE_IN_SLEEP | WDT_OPT_PAUSE_HALTED_BY_DBG);

	if (rc < 0) {
		LOG_ERR("hang_capture: wdt_setup failed (%d)", rc);
		return 0;
	}

	wdt_armed = true;
	LOG_INF("hang_capture armed: wdt timeout=%ums feed=%ums",
		CONFIG_CHARYBDIS_HANG_CAPTURE_TIMEOUT_MS,
		CONFIG_CHARYBDIS_HANG_CAPTURE_FEED_MS);
	return 0;
}

SYS_INIT(hang_capture_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif /* CONFIG_CHARYBDIS_HANG_CAPTURE */
