#!/bin/zsh

# =============================================================================
# CHARYBDIS ZMK FIRMWARE SERIAL DEBUG (USB CDC console)
# =============================================================================
# Captures the keyboard's own log stream over USB — panics, kernel/usage faults,
# BLE disconnect reason codes — to diagnose the weekend crash.
#
# Prereq: flash the `charybdis_right_debug` artifact (built with
#   CONFIG_ZMK_USB_LOGGING=y) to the RIGHT half, then keep it tethered via USB.
#
# By default, key and mouse activity is filtered out of both the terminal and
# the logfile -- the DBG stream logs keycodes/positions/typed characters, so an
# unfiltered log is effectively a keylogger. The default keeps connection and
# crash telemetry only. Pass -v/--verbose to include the key/mouse lines (do NOT
# leave a verbose capture lying around). Faults/panics/asserts/BLE disconnects
# are never filtered in either mode.
#
# Usage:
#   ./scripts/zmk_serial_debug.sh                 # auto-detect port, safe filter
#   ./scripts/zmk_serial_debug.sh -v              # include keystrokes (keylogger)
#   ./scripts/zmk_serial_debug.sh /dev/cu.usbmodemXXXX
#
# Exit: Ctrl-C.
# =============================================================================

set -u

BAUD=115200
LOG_DIR="${0:A:h}/../logs"

c_cyan="\033[1;36m"; c_green="\033[1;32m"; c_yellow="\033[1;33m"
c_red="\033[1;31m"; c_reset="\033[0m"

# ALLOWLIST (not a denylist). The default mode passes ONLY lines that match this
# pattern; everything else is dropped. This is deliberate:
#   1. Privacy: the DBG firehose logs HID keycodes, key positions, matrix
#      row/col, binding names and a hexdump of typed characters. A denylist is
#      whack-a-mole -- every new firmware debug category leaks more. An allowlist
#      can only ever emit the categories below, so key data can never slip in.
#   2. Signal: for a crash/disconnect hunt we only want connection state and
#      faults, which are logged at <inf>/<wrn>/<err> plus a few stable markers.
# To see the full stream (with keystrokes), run with -v/--verbose.
#
# NOTE: matches "Connected"/"Disconnected" (capital, ZMK's wording) but NOT bare
# "connect", so the key-spam line "No connection for passkey entry" is excluded.
# Added: split CONNECTION markers (not the notify/event lines — those carry the
# position bitmap), conn-param updates, and reboot/boot banners so a central
# reset is visible. Bare "split"/"position"/"peripheral_event" stay OUT (key data).
KEEP_RE='<inf>|<wrn>|<err>|BUILDSTAMP|Thread analyze|thread_analyzer|STACK:| unused |Disconnected|Connected|reason 0x|param|security|encrypt|bond|paired|pairing|profile|advertis|MTU|PHY|settings|Booting|Bootloader|Zephyr|reboot|sys_reboot| reset|panic|PANIC|fault|FAULT|assert|ASSERT|stack overflow|watchdog|brownout|BROWNOUT|split_central_conn|split_central_disconn|start_scan|stop_scan|le_param|conn_param|update_conn|supervision|Failed|failed to'

# --- parse args (flag and/or explicit port, any order) ------------------------
VERBOSE=0
PORT=""
for arg in "$@"; do
  case "$arg" in
    -v|--verbose) VERBOSE=1 ;;
    -*) echo "${c_red}Unknown option: $arg${c_reset}"; exit 1 ;;
    *) PORT="$arg" ;;
  esac
done

# --- locate the serial port ---------------------------------------------------
# IMPORTANT: use the /dev/cu.* (callout) device, NOT /dev/tty.* — on macOS the
# tty.* node blocks on open until carrier (DCD) is asserted, so `cat` hangs and
# captures nothing. cu.* is non-blocking and is the correct device for reading.
if [ -z "$PORT" ]; then
  PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -n1)
fi

if [ -z "$PORT" ] || [ ! -e "$PORT" ]; then
  echo "${c_red}No USB serial port found.${c_reset}"
  echo "  - Is the RIGHT half flashed with the *_debug firmware and plugged in?"
  echo "  - A bare nice_nano in bootloader mode shows as a disk, not a serial port."
  echo "  - Use a DATA usb cable, not charge-only."
  echo "  - List candidates:  ls /dev/cu.usbmodem*"
  exit 1
fi

# --- prep timestamped logfile -------------------------------------------------
mkdir -p "$LOG_DIR"
STAMP=$(date +%Y%m%d-%H%M%S)
LOG_FILE="$LOG_DIR/zmk-crash-$STAMP.log"

clear
echo "${c_cyan}=================================================================${c_reset}"
echo "${c_cyan}  CHARYBDIS ZMK FIRMWARE SERIAL DEBUG${c_reset}"
echo "  Port:    ${c_green}$PORT${c_reset} @ ${BAUD} baud"
echo "  Logfile: ${c_green}$LOG_FILE${c_reset}"
if [ "$VERBOSE" -eq 1 ]; then
  echo "  Filter:  ${c_red}OFF -- verbose: logs keystrokes/positions (keylogger!)${c_reset}"
else
  echo "  Filter:  ${c_green}allowlist -- connection+crash only; all key/mouse data dropped${c_reset}"
fi
echo "${c_cyan}=================================================================${c_reset}"
echo "  Watch for:"
echo "    ${c_red}***** USAGE FAULT *****${c_reset} / ${c_red}***** Kernel Panic *****${c_reset}  -> firmware crash + register dump"
echo "    ${c_yellow}<wrn> ... disconnected (reason 0xXX)${c_reset}              -> BLE link loss reason"
echo "    ${c_yellow}peripheral ... disconnected${c_reset}                       -> split half dropped"
echo ""
echo "  Leave this running (overnight/weekend). On crash the dump lands above"
echo "  AND in the logfile. Exit: ${c_yellow}Ctrl-C${c_reset}."
echo "${c_cyan}=================================================================${c_reset}"
echo ""

# macOS ships an old BSD `screen` without -Logfile, so capture with stty+cat|tee
# instead. This is one-way (read-only) capture -- exactly what log streaming
# needs -- and `tee` mirrors to the terminal and the logfile at once. Opening
# the port asserts DTR, which the ZMK USB CDC console needs before it streams.
#
# RECONNECT LOOP: the failure we're hunting drops the USB link (the central
# re-enumerates / reboots, or the port glitches). A plain `cat | grep | tee`
# exits on that EOF -- so the logger dies at the exact instant of the crash and
# misses the `Disconnected (reason 0x..)` line and the `Booting Zephyr` banner.
# Instead we loop: when the stream ends, mark it with a wall-clock stamp, wait
# for the port to reappear, and reattach -- all into the SAME logfile. One
# continuous capture across the crash; Ctrl-C still stops it.

# -----------------------------------------------------------------------------
# LOG LINE FORMAT (one event per line, fixed pipe-delimited columns)
#
#   2026-06-29 13:07:42 PDT | DEV  | [00:53:18.491,394] <inf> zmk: Endpoint ...
#   2026-06-29 13:07:45 PDT | LOGR | stream ended (USB drop/reboot) — reconnecting
#   └──── host local wall-clock ───┘   │       │   └─ payload
#                                      │       └──── DEV  = device firmware line
#                                      │              LOGR = this logger's own event
#                                      └─ column separator " | " (split on it)
#
# Why this shape: the first column is always system-LOCAL wall-clock, so "it
# crashed at 1:07pm" maps straight to a line. The DEV payload keeps the firmware's
# own [uptime] <level> zmk: text intact, so one line traces host-time -> device
# uptime -> log call site. Fixed columns + a stable " | " delimiter make it
# greppable (`grep ' | LOGR '`), awk-friendly (`-F' | '`), and easy for an LLM to
# parse. Local timezone is shown per line and also printed once in the banner.
# -----------------------------------------------------------------------------

# stamp_dev: strip the ANSI color codes the firmware emits (they corrupt the
# logfile and confuse parsers) AND prefix each line with a local-time stamp.
# Done in ONE perl process -- no per-line `date` fork -- so it keeps pace with
# the full DBG firehose in verbose mode without overflowing the tty buffer.
stamp_dev() {
  perl -ne 'BEGIN { $| = 1; use POSIX qw(strftime); }
            s/\e\[[0-9;]*m//g;                                   # strip ANSI SGR
            chomp(my $line = $_);
            print strftime("%Y-%m-%d %H:%M:%S %Z", localtime),
                  " | DEV  | ", $line, "\n";'
}

# log_event: emit one structured LOGR line -- colored to the terminal, plain to
# the logfile -- in the same column format as the device lines above.
log_event() {  # $1 = terminal color, $2 = message
  local ts; ts=$(date '+%Y-%m-%d %H:%M:%S %Z')
  printf "${1}%s | LOGR | %s${c_reset}\n" "$ts" "$2"
  printf  '%s | LOGR | %s\n' "$ts" "$2" >> "$LOG_FILE"
}

echo "  Time:    ${c_green}system-local${c_reset} ($(date '+%Z, UTC%z'))   Format: ${c_green}<local-time> | DEV|LOGR | <msg>${c_reset}"

# Build traceability: record which config commit THIS logger was launched from.
# The flashed firmware logs its own `BUILDSTAMP git=<sha>` at boot -- if the two
# git= values differ, the board is running a stale image (rebuild/reflash).
EXPECT_GIT=$(git -C "${0:A:h}/.." describe --always --dirty --tags 2>/dev/null || echo "nogit")
echo "  Build:   ${c_green}logger launched from config git=${EXPECT_GIT}${c_reset} -- expect matching firmware BUILDSTAMP"
echo "${c_green}--- streaming (Ctrl-C to stop; auto-reconnects across crashes) ---${c_reset}"
trap 'log_event "$c_yellow" "logger stopped (Ctrl-C)"; exit 0' INT
log_event "$c_cyan" "logger launched from config git=${EXPECT_GIT} (tap RIGHT reset to print firmware BUILDSTAMP for comparison)"

while true; do
  if [ ! -e "$PORT" ]; then
    log_event "$c_yellow" "waiting for $PORT (device down / rebooting)..."
    while [ ! -e "$PORT" ]; do sleep 0.5; done
    log_event "$c_green" "port back -- reattaching"
  fi

  # Configure the port: speed, 8N1, raw (no line processing), no echo.
  # CDC ACM ignores the baud rate but stty still wants a valid value.
  stty -f "$PORT" "$BAUD" cs8 -cstopb -parenb -echo raw 2>/dev/null \
    || stty -f "$PORT" "$BAUD" 2>/dev/null \
    || log_event "$c_yellow" "stty could not configure $PORT (continuing)"

  log_event "$c_green" "attached to $PORT @ ${BAUD} baud -- streaming"
  if [ "$VERBOSE" -eq 1 ]; then
    cat "$PORT" 2>/dev/null | stamp_dev | tee -a "$LOG_FILE"
  else
    # Allowlist: keep only connection/crash lines. Case-sensitive on purpose --
    # case-insensitive would let "PHY" match "physical" in the kscan spam.
    # grep runs BEFORE stamp_dev (ANSI codes don't sit inside the matched tokens).
    cat "$PORT" 2>/dev/null | grep --line-buffered -E "$KEEP_RE" | stamp_dev | tee -a "$LOG_FILE"
  fi

  # cat returned -> EOF = USB dropped (crash / re-enumerate / unplug).
  log_event "$c_red" "stream ended (USB drop / reboot) -- reconnecting"
  sleep 0.5
done
