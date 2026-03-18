# External Integrations

**Analysis Date:** 2026-03-18

## APIs & External Services

**Firmware Repositories:**
- ZMK Firmware (zmkfirmware/zmk) - Main keyboard framework
  - SDK/Client: west package manager
  - Source: `https://github.com/zmkfirmware/zmk`
  - Location: `config/west.yml` lines 8-11
  - Purpose: Core ZMK layers, key bindings, BLE driver stubs, input event processing

- PMW3610 Driver (badjeff/zmk-pmw3610-driver) - Trackball sensor driver
  - SDK/Client: west dependency
  - Source: `https://github.com/badjeff/zmk-pmw3610-driver`
  - Location: `config/west.yml` lines 12-14
  - Purpose: SPI interface to Pixart PMW3610 optical trackball sensor

**CI/CD:**
- GitHub Actions - Automated firmware builds
  - Triggered on: push, pull_request, workflow_dispatch
  - Workflow file: `.github/workflows/build.yml`
  - Uses shared workflow from ZMK upstream
  - Produces `.uf2` firmware artifacts

## Bluetooth Connectivity

**BLE Protocol:**
- Bluetooth 5.0 Low Energy
  - MCU Stack: Nordic nRF52840 native BLE controller
  - Configuration: `config/charybdis.conf`
  - Features:
    - Split keyboard pairing (central role on right half)
    - Experimental connection improvements enabled
    - TX Power: +8 dBm (maximum)
    - Connection interval: 9ms preferred
    - Latency: 16 peripheral latency slots
    - Buffer tuning: 32 ACL TX buffers, 32 L2CAP TX buffers

- Battery Monitoring:
  - Central device fetches remote peripheral battery level
  - Configuration: `CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING=y`
  - Proxy feature enabled: `CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_PROXY=y`
  - Voltage measurement: nRF-based internal VDDH divider

## Hardware Interfaces

**SPI Bus:**
- Trackball Sensor Communication
  - Device: PMW3610 optical sensor
  - Bus: SPI0 (native Nordic SPIM)
  - Pins (defined in `config/boards/shields/charybdis/pmw3610.dtsi`):
    - SCK: GPIO P0.08
    - MOSI: GPIO P0.17
    - MISO: GPIO P0.17
    - CS: GPIO P0.20 (active low)
    - IRQ: GPIO P0.06 (active low with pull-up)
  - Frequency: 2 MHz
  - CPI: 800 (configurable via `cpi` device tree property)

**GPIO Matrix:**
- Keyboard Scanning (8x4 matrix, 12 cols per side)
  - Row pins: P0.18, P0.05, P0.04, P0.09
  - Column pins: P0.19, P0.20, P0.10, P0.06, P0.07, P0.08
  - Diode direction: Row-to-column (anti-ghosting)
  - Scan mode: GPIO-based matrix scanner (zmk,kscan-gpio-matrix)

**USB Interface:**
- Serial UART (optional on right half)
  - Snippet: `studio-rpc-usb-uart`
  - Purpose: ZMK Studio RPC protocol for real-time configuration
  - Enabled when: Right half built with `CONFIG_ZMK_STUDIO=y`

## Data Storage

**Non-Volatile Storage:**
- MCU Flash (nRF52840)
  - Used for: Firmware, configuration storage, bonding information
  - Size: 1MB (shared with firmware)
  - Manager: ZMK EEPROM-like storage layer

**Runtime State:**
- RAM (nRF52840)
  - Keyboard state machine buffers
  - Behavior queue: 512 events max
  - BLE connection context
  - Trackball sample buffers

## Input Devices

**Trackball Sensor:**
- Pixart PMW3610
  - Location: `config/boards/shields/charybdis/pmw3610.dtsi`
  - Input events: Relative X/Y motion + optional wheel
  - Event codes:
    - `INPUT_REL_X` = 0x00 (relative X movement)
    - `INPUT_REL_Y` = 0x01 (relative Y movement)
    - `INPUT_REL_WHEEL` = 0x08 (scroll wheel)
  - Orientation: swap-xy enabled, invert-x, invert-y
  - Sensitivity: Configurable via `cpi` (counts per inch)
  - Processor chain: `zip_xy_scaler` for sensitivity adjustment
  - Scroll layer: Layer 5, uses 1:20 scaling for scroll events

**Mechanical Keyboard Matrix:**
- 5x6 standard layout per half (split ergonomic)
- Transform definitions: `config/boards/shields/charybdis/charybdis-layouts.dtsi`
  - Default 6-column layout
  - 5-column variant available (col-offset adjustment)

## Authentication & Identity

**Bluetooth Pairing:**
- BLE bonding/pairing
  - Central role: Right half (coordinates with left peripheral)
  - Bonding info stored on MCU flash
  - Standard nRF52840 BLE security procedures
  - No centralized auth backend

## Monitoring & Observability

**Logging:**
- Zephyr logging framework (optional, compile-time configurable)
- Serial/UART output on right half (if USB variant enabled)
- No external logging service

**Debug Interface:**
- ZMK Studio RPC Protocol (optional)
  - Endpoint: USB serial (right half only)
  - Build flag: `CONFIG_ZMK_STUDIO=y`
  - Allows live keymap inspection and testing

**Behavior Monitoring:**
- Input event queue monitoring
- Battery level polling (central to peripheral)
- Connection state tracking (via standard BLE callbacks)

## CI/CD & Deployment

**Build Pipeline:**
- GitHub Actions workflow: `.github/workflows/build.yml`
- Triggers: push, pull_request, workflow_dispatch
- Shared workflow: `zmkfirmware/zmk/.github/workflows/build-user-config.yml@main`
- Matrix builds:
  - `nice_nano` board with `charybdis_left` shield
  - `nice_nano` board with `charybdis_right` shield (includes ZMK Studio RPC)
  - `nice_nano` board with `settings_reset` shield (factory reset tool)

**Artifact Storage:**
- GitHub Actions artifact storage (workflow artifacts)
- Format: `.uf2` files (UF2 bootloader format)
- Distribution: Downloaded from GitHub releases or Actions workflow

**Flashing Method:**
- UF2 bootloader on Nice!Nano V2
- Drag-and-drop firmware update to USB mass storage
- No external firmware provisioning service

## Webhooks & Callbacks

**Incoming:**
- None detected

**Outgoing:**
- None detected

## Hardware Integration Points

**Nice!Nano V2 Board Support:**
- nRF52840 MCU integration
- Pin definitions via Zephyr board files (imported via west)
- Battery voltage divider (VDDH measurement)
- No external power management ICs

**Charybdis Shield Definition:**
- File: `config/boards/shields/charybdis/charybdis.zmk.yml`
- Type: ZMK shield (requires Pro Micro compatible board)
- Features: keys, pointer (trackball)
- Variants: charybdis_left, charybdis_right
- URL: https://github.com/Bastardkb/Charybdis/

---

*Integration audit: 2026-03-18*
