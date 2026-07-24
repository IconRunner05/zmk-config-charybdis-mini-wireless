/*
 * Boot-time build fingerprint.
 *
 * Logs one line at APPLICATION init so a serial capture can be traced back to a
 * single build. The line is emitted at <inf> and carries a stable "BUILDSTAMP"
 * anchor, so scripts/zmk_serial_debug.sh keeps it (allowlist) regardless of log
 * level. Compare the git= field here against the "logger expects" line the
 * script prints at start: a mismatch means the flashed image is stale.
 *
 *   <inf> build_stamp: BUILDSTAMP git=1f6ab77 built='Jul  1 2026 13:51:20'
 *
 * git=  config-repo `git describe --always --dirty --tags` (nogit if unknown).
 * built= compiler __DATE__/__TIME__ of THIS file — always present, unique per
 *        rebuild, so two builds off the same commit are still distinguishable.
 */

#include <zephyr/init.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(build_stamp, LOG_LEVEL_INF);

#ifndef CHARYBDIS_GIT_DESC
#define CHARYBDIS_GIT_DESC "nogit"
#endif

static int charybdis_log_build_stamp(void)
{
	LOG_INF("BUILDSTAMP git=%s built='%s %s'", CHARYBDIS_GIT_DESC, __DATE__, __TIME__);
	return 0;
}

SYS_INIT(charybdis_log_build_stamp, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
