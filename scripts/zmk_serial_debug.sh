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
# Usage:
#   ./scripts/zmk_serial_debug.sh           # auto-detect port, log to ./logs/
#   ./scripts/zmk_serial_debug.sh /dev/tty.usbmodemXXXX
#
# Exit screen: Ctrl-A then k  (or Ctrl-A then \).
# =============================================================================

set -u

BAUD=115200
LOG_DIR="${0:A:h}/../logs"

c_cyan="\033[1;36m"; c_green="\033[1;32m"; c_yellow="\033[1;33m"
c_red="\033[1;31m"; c_reset="\033[0m"

# --- locate the serial port ---------------------------------------------------
PORT="${1:-}"
if [ -z "$PORT" ]; then
  # nice_nano enumerates as /dev/tty.usbmodem* on macOS
  PORT=$(ls /dev/tty.usbmodem* 2>/dev/null | head -n1)
fi

if [ -z "$PORT" ] || [ ! -e "$PORT" ]; then
  echo "${c_red}No USB serial port found.${c_reset}"
  echo "  - Is the RIGHT half flashed with the *_debug firmware and plugged in?"
  echo "  - A bare nice_nano in bootloader mode shows as a disk, not a tty."
  echo "  - List candidates:  ls /dev/tty.usbmodem*"
  exit 1
fi

# --- dependency check ---------------------------------------------------------
if ! command -v screen >/dev/null 2>&1; then
  echo "${c_red}'screen' not installed.${c_reset}  brew install screen"
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
echo "${c_cyan}=================================================================${c_reset}"
echo "  Watch for:"
echo "    ${c_red}***** USAGE FAULT *****${c_reset} / ${c_red}***** Kernel Panic *****${c_reset}  -> firmware crash + register dump"
echo "    ${c_yellow}<wrn> ... disconnected (reason 0xXX)${c_reset}              -> BLE link loss reason"
echo "    ${c_yellow}peripheral ... disconnected${c_reset}                       -> split half dropped"
echo ""
echo "  Leave this running (overnight/weekend). On crash the dump lands above"
echo "  AND in the logfile. Exit screen: ${c_yellow}Ctrl-A then k${c_reset}."
echo "${c_cyan}=================================================================${c_reset}"
echo ""

# -L enables logging, -Logfile sets the path, -fn disables flow control so a
# crash-time burst is not throttled.
exec screen -L -Logfile "$LOG_FILE" -fn "$PORT" "$BAUD"
