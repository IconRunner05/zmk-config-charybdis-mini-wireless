# Testing Patterns

**Analysis Date:** 2026-03-18

## Test Framework

**Runner:**
- GitHub Actions via ZMK upstream workflow
- No local test runner detected

**Assertion Library:**
- Not applicable - ZMK builds firmware binaries, not unit tests

**Run Commands:**
```bash
# Build firmware (via GitHub Actions)
# Triggered on push, pull request, workflow_dispatch

# Manual build (requires ZMK SDK setup)
west build -b nice_nano -d build/left -- -DSHIELD=charybdis_left
west build -b nice_nano -d build/right -- -DSHIELD=charybdis_right -DCONFIG_ZMK_STUDIO=y
```

## Test File Organization

**Location:**
- No unit tests in this codebase - firmware configuration project
- Validation happens through GitHub Actions build process (`.github/workflows/build.yml`)

**Naming:**
- Not applicable - no test files

**Structure:**
```
.github/
├── workflows/
    └── build.yml          # CI/CD configuration
```

## Test Structure

**Build Validation:**
Build process validates:
- Device tree source (.dtsi) syntax
- Configuration (.conf) validity
- Keymap (.keymap) correctness
- Overlay (.overlay) compatibility

**Patterns:**
- GitHub Actions delegates to upstream ZMK workflow: `zmkfirmware/zmk/.github/workflows/build-user-config.yml@main`
- Builds two firmware variants: `charybdis_left` and `charybdis_right`
- Right-side variant includes Studio RPC support: `-DCONFIG_ZMK_STUDIO=y`

## Validation Strategy

**Device Tree Validation:**
- DTS compiler validates syntax and cross-references
- Device node declarations checked against bindings
- GPIO pin assignments validated (no conflicts)
- SPI configuration validated against hardware capabilities

**Configuration Validation:**
```bash
# Example: charybdis_right.conf validates
CONFIG_ZMK_SPLIT=y                              # Split keyboard enabled
CONFIG_ZMK_SPLIT_BLE_ROLE_CENTRAL=y            # Right is central
CONFIG_ZMK_KEYBOARD_NAME="Charybdis"           # Name configured
CONFIG_ZMK_POINTING=y                          # Mouse support enabled
CONFIG_PMW3610_ALT=y                           # Trackball driver enabled
CONFIG_SPI=y                                   # SPI enabled for trackball
```

**Keymap Validation:**
- Layer definitions check for duplicate names
- Key position bindings validated against matrix transform
- Custom behavior declarations checked for correct binding cells: `#binding-cells = <2>`
- Tapping term values in milliseconds: `tapping_term_ms = <200>`

## Mocking

**Framework:** None - firmware project uses hardware drivers

**Patterns:**
- Device nodes can be disabled via overlay: `status = "disabled"` (see `charybdis_left.overlay` disabling trackball and SPI)
- Configuration conditionals control feature compilation: `CONFIG_ZMK_EXT_POWER`, `CONFIG_PMW3610_ALT`

**What to Mock:**
- Trackball sensor (PMW3610) disabled on left half via overlay
- SPI bus disabled on left half to save power
- Features configured per-variant via `-DSHIELD` and `-DCONFIG_*` flags

**What NOT to Mock:**
- Core ZMK behavior system (handled by upstream)
- Zephyr RTOS drivers (handled by upstream)
- Keyboard matrix scanning (validated by build)

## Fixtures and Factories

**Test Data:**
- Physical layout definitions in `charybdis-layouts.dtsi`:
  ```dtsi
  keys = <&key_physical_attrs 100 100 0 38 0 0 0>
         , <&key_physical_attrs 100 100 100 38 0 0 0>
         ...
  ```
- Key position mapping in `charybdis.dtsi`:
  ```dtsi
  map = <
    RC(0,0) RC(0,1) RC(0,2) ... RC(0,11) RC(0,10) ...
    ...
  >;
  ```

**Location:**
- Physical layouts: `config/boards/shields/charybdis/charybdis-layouts.dtsi`
- Matrix transforms: `config/boards/shields/charybdis/charybdis.dtsi`
- Key bindings: `config/charybdis.keymap`

## Coverage

**Requirements:** None enforced - firmware project

**Validation Approach:**
- Compile-time validation only (DTS compiler, Kconfig processor)
- Runtime tested via actual hardware flashing and manual testing
- Build variants ensure both left and right halves compile correctly

## Test Types

**Compilation Tests:**
- Validates device tree syntax and references
- Checks configuration option compatibility
- Ensures keymap definitions are well-formed
- Verifies all includes resolve correctly

**Configuration Tests:**
```bash
# charybdis_left build:
# - Validates CONFIG_ZMK_SPLIT=y
# - Validates trackball disabled: status = "disabled"
# - Validates SPI disabled: status = "disabled"

# charybdis_right build:
# - Validates CONFIG_ZMK_SPLIT=y and CONFIG_ZMK_SPLIT_BLE_ROLE_CENTRAL=y
# - Validates CONFIG_ZMK_POINTING=y and CONFIG_PMW3610_ALT=y
# - Validates SPI enabled: status = "okay"
# - Validates Studio RPC: -DCONFIG_ZMK_STUDIO=y
```

**Integration Tests:**
- Not automated - requires hardware
- Manual testing: flash firmware to each half, verify wireless pairing, test all keymaps
- Trackball testing on right half only
- Battery level reporting on both halves

**E2E Tests:**
- Not applicable - firmware project

## Build Matrix

**Configuration Variants:**
```yaml
# From build.yaml
- board: nice_nano
  shield: charybdis_left

- board: nice_nano
  shield: charybdis_right
  snippet: studio-rpc-usb-uart
  cmake-args: -DCONFIG_ZMK_STUDIO=y

- board: nice_nano
  shield: settings_reset
```

**CI Triggers:**
- Push events
- Pull requests
- Manual workflow dispatch

## Hardware-Specific Validation

**Trackball (PMW3610-ALT):**
- Enabled only on right half: `config/boards/shields/charybdis/charybdis_right.conf`
- Sensitivity: `CONFIG_PMW3610_ALT=y` with `cpi = <800>` in `pmw3610.dtsi`
- Power tuning: `CONFIG_PMW3610_ALT_RUN_DOWNSHIFT_TIME_MS=3264`
- Orientation: `swap-xy`, `invert-x`, `invert-y` configured

**Bluetooth Configuration:**
```conf
# From charybdis.conf (both halves)
CONFIG_BT_CTLR_TX_PWR_PLUS_8=y
CONFIG_BT_PERIPHERAL_PREF_MAX_INT=9
CONFIG_BT_PERIPHERAL_PREF_LATENCY=16
CONFIG_BT_BUF_ACL_TX_COUNT=32
CONFIG_BT_L2CAP_TX_BUF_COUNT=32
CONFIG_ZMK_BLE_EXPERIMENTAL_CONN=y
CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING=y
CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_PROXY=y
CONFIG_ZMK_BEHAVIORS_QUEUE_SIZE=512
```

## Manual Testing Checklist

**Pre-build Validation:**
- Verify `charybdis.keymap` syntax correct (no missing semicolons)
- Check `charybdis.conf` for obsolete options (e.g., `CONFIG_PMW3610_ALT_INVERT_*`)
- Ensure overlay files reference valid nodes from base DTSi

**Post-build Validation:**
- Flash left and right .uf2 files to respective halves
- Verify wireless pairing completes
- Test base layer typing
- Test NUM layer (numbers, symbols, Bluetooth switching)
- Test NAV layer (navigation, F-keys, bootloader access)
- Test FUN layer (function keys, audio control)
- Verify trackball functionality on right half only
- Test combo key sequences (Delete, Backspace, Enter, etc.)
- Verify custom behaviors (hold-tap timing on NUM/NAV layers)

---

*Testing analysis: 2026-03-18*
