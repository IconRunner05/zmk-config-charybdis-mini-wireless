#!/bin/zsh

# =============================================================================
# CHARYBDIS WIRELESS TRACKBALL DEBUG CONSOLE (v1.4)
# =============================================================================
# Streams, cleans, and translates macOS Bluetooth daemon logs for:
# Device UUID: 16B237D6-4DB2-6D6C-5F03-223F2057868D
# =============================================================================

# Parse command line flags
ERRORS_ONLY=0
for arg in "$@"; do
  if [[ "$arg" == "--errors-only" || "$arg" == "-e" || "$arg" == "--quiet" || "$arg" == "-q" ]]; then
    ERRORS_ONLY=1
  fi
done

# Root privilege check
if [ "$EUID" -ne 0 ]; then
  echo "\033[1;33m⚠️  This script requires ROOT privileges to stream system logs.\033[0m"
  echo "Please authenticate when prompted:"
  # Forward arguments to the sudo-executed instance
  sudo "$0" "$@"
  exit $?
fi

DEVICE_UUID="16B237D6-4DB2-6D6C-5F03-223F2057868D"

clear
echo "\033[1;36m================================================================="
echo "  🔮 CHARYBDIS MINI WIRELESS — TRACKBALL TELEMETRY CONSOLE (v1.4)"
echo "  Tracking Device: $DEVICE_UUID"
if [ "$ERRORS_ONLY" -eq 1 ]; then
  echo "  Mode:            \033[1;33m⚠️  Quiet (Errors, Warnings & Link Degradation Only)\033[1;36m"
else
  echo "  Mode:            \033[1;32m📊 Verbose (Full Live Log Stream)\033[1;36m"
fi
echo "=================================================================\033[0m"
if [ "$ERRORS_ONLY" -eq 1 ]; then
  echo "  This terminal will remain silent until an issue is detected."
else
  echo "  Move your trackball to start receiving real-time logs..."
fi
echo "  Press \033[1;31mCtrl + C\033[0m to exit at any time."
echo ""

# Stream logs through awk to colorize, translate, and format
sudo log stream --process bluetoothd --level debug | awk -v uuid="$DEVICE_UUID" -v err_only="$ERRORS_ONLY" '
BEGIN {
    # Define custom colors
    c_cyan    = "\033[36m"
    c_green   = "\033[32m"
    c_yellow  = "\033[33m"
    c_red     = "\033[31m"
    c_blue    = "\033[34m"
    c_magenta = "\033[35m"
    c_reset   = "\033[0m"
    c_bold    = "\033[1m"
}

# 1. Ignore background metrics, power log reporting, and discovery daemons
/CBMetricsDaemon|PowerLog|CBDiscovery|milod|sharingd|CBAccessoryDaemon/ {
    next
}

# 2. Match Direct GATT events matching the trackball UUID
$0 ~ uuid {
    # Suppress raw data events entirely in errors-only mode
    if (err_only == 1) {
        next
    }

    split($2, t_parts, ".")
    time_stamp = t_parts[1]
    
    if ($0 ~ /Dispatching indication/) {
        print c_cyan "[" time_stamp "]" c_reset " " c_green "⚡ [DATA]" c_reset " Motion packet dispatched to macOS input queue"
    }
    else if ($0 ~ /GATT indication/) {
        # Silence secondary GATT session dispatch lines
        next
    }
    else {
        # Catch-all for any other trackball specific events
        print c_cyan "[" time_stamp "]" c_reset " " c_magenta "⚙️ [SYSTEM]" c_reset " " substr($0, index($0, "bluetoothd:"))
    }
    fflush()
}

# 3. Match general Link Quality reports (RSSI, coexistence, packets)
/Server.Core] Le \[/ {
    split($2, t_parts, ".")
    time_stamp = t_parts[1]
    
    # Robust BLE connection metrics parsing
    match($0, /rssi -?[0-9]+/)
    rssi_str = substr($0, RSTART, RLENGTH)
    split(rssi_str, rssi_parts, " ")
    rssi = rssi_parts[2] + 0
    
    tx_val = "tx [S=0:F=0]"
    tx_fails = 0
    if (match($0, /tx\s*\[S=[^\]]+\]/)) {
        tx_val = substr($0, RSTART, RLENGTH)
        # Parse TX Failure count to flag link issues
        if (match(tx_val, /F=\s*[0-9]+/)) {
            split(substr(tx_val, RSTART, RLENGTH), tx_f_parts, "=")
            tx_fails = tx_f_parts[2] + 0
        }
    }
    
    rx_val = "rx [S=0:F=0]"
    rx_fails = 0
    if (match($0, /rx\s*\[S=[^\]]+\]/)) {
        rx_val = substr($0, RSTART, RLENGTH)
        # Parse RX Failure count to flag link issues
        if (match(rx_val, /F=\s*[0-9]+/)) {
            split(substr(rx_val, RSTART, RLENGTH), rx_f_parts, "=")
            rx_fails = rx_f_parts[2] + 0
        }
    }
    
    # Translate signal quality
    sig_color = c_green
    sig_text = "Excellent"
    if (rssi < -80) {
        sig_color = c_red
        sig_text = "Critical (Prone to Lag)"
    } else if (rssi < -68) {
        sig_color = c_yellow
        sig_text = "Weak (Possible Stutter)"
    } else if (rssi < -50) {
        sig_color = c_cyan
        sig_text = "Good"
    }
    
    # In quiet mode, ONLY print link telemetry if signal is weak/critical or packet failures occur
    if (err_only != 1 || rssi < -68 || tx_fails > 0 || rx_fails > 0) {
        print c_cyan "[" time_stamp "]" c_reset " " c_blue "📊 [LINK]" c_reset " Signal: " sig_color rssi " dBm (" sig_text ")" c_reset " | " tx_val " | " rx_val
    }
    fflush()
}

# 4. Match Bluetooth scanning activity
/LE.Scan|Scanning/ {
    # Suppress background scanning traces in errors-only mode
    if (err_only == 1) {
        next
    }

    split($2, t_parts, ".")
    time_stamp = t_parts[1]
    
    scan_msg = substr($0, index($0, "bluetoothd:"))
    gsub("bluetoothd: ", "", scan_msg)
    
    print c_cyan "[" time_stamp "]" c_reset " " c_magenta "📡 [SCAN]" c_reset " " scan_msg
    fflush()
}

# 5. Match BLE topology / latency renegotiation logs cleanly
/Le topology|Connecting with interval/ {
    # Suppress topology updates in errors-only mode to keep it perfectly silent
    if (err_only == 1) {
        next
    }

    split($2, t_parts, ".")
    time_stamp = t_parts[1]
    
    topo_msg = substr($0, index($0, "bluetoothd:"))
    gsub("bluetoothd: ", "", topo_msg)
    
    # Format and colorize topology params
    gsub("interval:", c_green "interval:" c_reset, topo_msg)
    gsub("latency:", c_yellow "latency:" c_reset, topo_msg)
    
    print c_cyan "[" time_stamp "]" c_reset " " c_cyan "🌐 [TOPOLOGY]" c_reset " " topo_msg
    fflush()
    next
}

# 6. Match warning or error logs indicating actual packet drops or congested links
/congested|throttle/ || ($0 ~ /dropped/ && $0 !~ /0\s*dropped|dropped\s*0/) {
    split($2, t_parts, ".")
    time_stamp = t_parts[1]
    
    print c_cyan "[" time_stamp "]" c_reset " " c_red "⚠️  [WARNING]" c_reset " " substr($0, index($0, "bluetoothd:"))
    fflush()
}
'
