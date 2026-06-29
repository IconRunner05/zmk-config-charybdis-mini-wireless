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
# By default the noisy trackball/mouse debug lines are filtered out of both the
# terminal and the logfile so a crash is not buried in mouse spam. Pass
# -v/--verbose to keep everything (firmware fault dumps are NEVER filtered --
# the patterns are mouse-specific).
#
# Usage:
#   ./scripts/zmk_serial_debug.sh                 # auto-detect port, filtered
#   ./scripts/zmk_serial_debug.sh -v              # keep trackball spam too
#   ./scripts/zmk_serial_debug.sh /dev/cu.usbmodemXXXX
#
# Exit: Ctrl-C.
# =============================================================================

set -u

BAUD=115200
LOG_DIR="${0:A:h}/../logs"

c_cyan="\033[1;36m"; c_green="\033[1;32m"; c_yellow="\033[1;33m"
c_red="\033[1;31m"; c_reset="\033[0m"

# Trackball/mouse spam to drop unless --verbose. Mouse-specific only, so panics,
# faults, BLE disconnects and split events always pass through.
SPAM_RE='apply_config: LISTENER INDEX|scale_val:|zmk_hid_mouse_movement_set|zmk_hid_mouse_scroll_set'

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
  echo "  Filter:  ${c_yellow}off (verbose -- trackball spam included)${c_reset}"
else
  echo "  Filter:  ${c_green}on (trackball/mouse spam dropped; -v to keep)${c_reset}"
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

# Configure the port: set speed, 8N1, raw (no line processing), no echo.
# CDC ACM ignores the baud rate but stty still wants a valid value.
stty -f "$PORT" "$BAUD" cs8 -cstopb -parenb -echo raw 2>/dev/null \
  || stty -f "$PORT" "$BAUD" 2>/dev/null \
  || echo "${c_yellow}warning: stty could not configure $PORT (continuing anyway)${c_reset}"

echo "${c_green}--- streaming (Ctrl-C to stop) ---${c_reset}"
# Ctrl-C tears down the pipeline; tee -a appends if the file already exists.
# --line-buffered keeps grep flushing each line live (and before a crash).
if [ "$VERBOSE" -eq 1 ]; then
  cat "$PORT" | tee -a "$LOG_FILE"
else
  cat "$PORT" | grep --line-buffered -vE "$SPAM_RE" | tee -a "$LOG_FILE"
fi
