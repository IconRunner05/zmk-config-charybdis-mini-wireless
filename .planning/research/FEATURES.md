# Feature Landscape: ZMK Zephyr 4.x Migration

**Domain:** Embedded firmware migration (ZMK keyboard firmware, Zephyr RTOS upgrade)
**Researched:** 2026-03-18
**Overall Confidence:** MEDIUM (web verification tools unavailable; analysis based on codebase inspection and training data knowledge of ZMK/Zephyr changes through early 2025)

## Research Limitations

Web search and fetch tools were unavailable during this research. All findings are based on:
1. Direct inspection of the current codebase configuration
2. Training data knowledge of ZMK and Zephyr changes (cutoff ~May 2025)
3. Pattern analysis of the specific configuration files

**Critical recommendation:** Before implementing any fix, verify each item against the current ZMK documentation at zmk.dev and the ZMK GitHub repository. Flag items marked LOW confidence for mandatory verification.

---

## Must-Fix: Causes Device Not to Enumerate

These issues would cause a complete failure to enumerate over USB or BLE. The device showing zero activity after flash points to a fundamental initialization failure.

### MF-1: Board Name `nice_nano` vs `nice_nano_v2` (CRITICAL)

| Attribute | Detail |
|-----------|--------|
| **Confidence** | HIGH |
| **Evidence** | `build.yaml` line 16: `board: nice_nano` |
| **Symptom** | Complete failure to enumerate |
| **Complexity** | Low |

**What changed:** In the Zephyr 4.x era, ZMK consolidated the nice!nano board definitions. The board identifier `nice_nano` (without version suffix) historically mapped to the nice!nano v1, which uses the nRF52832. The nice!nano v2 uses the nRF52840 and has a different board definition with different USB and BLE capabilities.

**Why this breaks everything:** The `build.yaml` specifies `board: nice_nano` for all three builds (left, right, settings_reset). Commit `7601b0e` explicitly removed the version tag. If ZMK's current main branch requires `nice_nano_v2` as the board identifier for v2 hardware (which uses nRF52840), building with `nice_nano` would produce firmware targeting the wrong SoC. A firmware compiled for nRF52832 flashed onto nRF52840 hardware would fail to initialize peripherals, USB, BLE -- everything.

**The fix:** Change `build.yaml` to use `nice_nano_v2` for all board entries:
```yaml
include:
  - board: nice_nano_v2
    shield: charybdis_left
  - board: nice_nano_v2
    shield: charybdis_right
    snippet: studio-rpc-usb-uart
    cmake-args: -DCONFIG_ZMK_STUDIO=y
  - board: nice_nano_v2
    shield: settings_reset
```

**Verification needed:** Confirm the exact board identifier string accepted by current ZMK main. It may have changed to `nice_nano/nrf52840` or similar Zephyr hardware model v2 format. Check the ZMK repo at `app/boards/` for the current directory/identifier.

### MF-2: Zephyr Hardware Model v2 Board Identifier Format

| Attribute | Detail |
|-----------|--------|
| **Confidence** | MEDIUM |
| **Evidence** | Zephyr 3.7+ introduced hardware model v2 |
| **Symptom** | Build failure or wrong target binary |
| **Complexity** | Low |

**What changed:** Zephyr 3.7 (and solidified in 4.x) introduced "hardware model v2" which changes board identifiers from flat names like `nice_nano_v2` to a hierarchical format: `nice_nano_v2/nrf52840`. ZMK adopted this in their main branch.

**Why this matters:** If ZMK main now requires the new format, the old-style `nice_nano_v2` may still work (Zephyr provides backward compatibility) but the build system behavior may differ. The key concern is whether the build resolves to the correct board definition at all.

**The fix:** After verifying MF-1, check if the board identifier needs the SoC qualifier:
```yaml
board: nice_nano_v2    # Old style - may still work
board: nice_nano_v2/nrf52840  # New Zephyr HWMv2 style
```

**Verification needed:** Check what ZMK's `build-user-config.yml` workflow expects. The shared workflow at `zmkfirmware/zmk/.github/workflows/build-user-config.yml@main` may handle the translation, or may require the new format.

### MF-3: Devicetree `label` Property Deprecated in Zephyr 4.x

| Attribute | Detail |
|-----------|--------|
| **Confidence** | MEDIUM |
| **Evidence** | `charybdis.dtsi` line 38: `label = "KSCAN";` |
| **Symptom** | Possible build failure or device init failure |
| **Complexity** | Low |

**What changed:** Zephyr deprecated the `label` property in devicetree nodes starting in Zephyr 3.x and removed support in 4.x. The `label` property was replaced by using node names and `DT_NODELABEL()` macros in C code.

**Current code issue:** The kscan node in `charybdis.dtsi` line 38 has `label = "KSCAN";`. If ZMK's current Zephyr version rejects or ignores this, the keyboard scan matrix may fail to initialize, which would prevent USB HID from having anything to report (though this alone might not prevent enumeration).

**The fix:** Remove the `label = "KSCAN";` line from `charybdis.dtsi`. ZMK's framework locates the kscan via the chosen node or devicetree node label, not the deprecated label property.

**Risk assessment:** This is more likely to cause a build warning than a complete enumeration failure on its own, but in combination with other issues it could contribute to init failure. Flagging as must-fix because it is a known deprecation.

### MF-4: `CONFIG_ZMK_BLE_EXPERIMENTAL_CONN` Removal or Rename

| Attribute | Detail |
|-----------|--------|
| **Confidence** | LOW |
| **Evidence** | `charybdis.conf` line 9: `CONFIG_ZMK_BLE_EXPERIMENTAL_CONN=y` |
| **Symptom** | Build failure (unrecognized Kconfig) or runtime BLE failure |
| **Complexity** | Low |

**What changed:** ZMK has been iterating on BLE connection handling. The `CONFIG_ZMK_BLE_EXPERIMENTAL_CONN` setting was an experimental flag that may have been graduated to default behavior (removed as a toggle) or renamed in current ZMK main.

**Why this could break things:** If this Kconfig symbol no longer exists, the build system behavior depends on how Zephyr handles unknown Kconfig. In some configurations, an unknown Kconfig symbol causes a build error. In others, it is silently ignored but the BLE connection parameters it controlled may now use different defaults that are incompatible with the split keyboard setup.

**The fix:** Check if `CONFIG_ZMK_BLE_EXPERIMENTAL_CONN` exists in current ZMK main. If removed:
- Remove the line from `charybdis.conf`
- The BLE tuning parameters below it (`CONFIG_BT_PERIPHERAL_PREF_MAX_INT`, `CONFIG_BT_BUF_ACL_TX_COUNT`, `CONFIG_BT_L2CAP_TX_BUF_COUNT`) should be reviewed for current validity

**Verification needed:** Search the current ZMK source for this Kconfig symbol.

---

## Should-Fix: Causes Functionality Loss

These issues would not prevent enumeration but would break specific features.

### SF-1: PMW3610 Driver API Compatibility with Current ZMK Main

| Attribute | Detail |
|-----------|--------|
| **Confidence** | MEDIUM |
| **Evidence** | `west.yml` pins `zmk-pmw3610-driver` to `main`, `charybdis_right.conf` uses `CONFIG_PMW3610_ALT=y` |
| **Symptom** | Trackball non-functional, possible build failure |
| **Complexity** | Medium |

**What changed:** The badjeff `zmk-pmw3610-driver` has been actively evolving. The driver already transitioned from `CONFIG_PMW3610_ALT_INVERT_X/Y` Kconfig options to devicetree properties (`swap-xy`, `invert-x`, `invert-y`), which this codebase has already partially adapted to.

**Current risks:**
1. The driver's `main` branch may have further API changes since the last working build
2. The `compatible = "pixart,pmw3610-alt"` string may have changed
3. The input event reporting interface may have changed with Zephyr 4.x input subsystem updates
4. The `CONFIG_PMW3610_ALT_*` Kconfig namespace may have been restructured

**The fix:**
- Pin `zmk-pmw3610-driver` to a known-working commit hash in `west.yml` rather than tracking `main`
- Verify the driver builds against current ZMK main
- Check if `compatible` string has changed

**Verification needed:** Check badjeff's GitHub repo for recent commits, especially any tagged releases or migration notes.

### SF-2: Input Processor / Trackball Listener API Changes

| Attribute | Detail |
|-----------|--------|
| **Confidence** | MEDIUM |
| **Evidence** | `charybdis.keymap` lines 13-27 use `zmk,input-listener` with `input-processors` |
| **Symptom** | Trackball movement not registered or incorrectly processed |
| **Complexity** | Medium |

**What changed:** ZMK's input processor system has been evolving. The keymap uses:
- `compatible = "zmk,input-listener"` with `device = <&trackball>`
- `input-processors = <&zip_xy_scaler 1 2>`
- `<&zip_xy_to_scroll_mapper>` and `<&zip_scroll_transform>`
- Layer-scoped input processor overrides (`scroll { layers = <5>; ... }`)

These APIs may have changed in naming, parameter format, or behavior between Zephyr 3.x and 4.x era ZMK.

**Specific concerns:**
1. The `#include <input/processors.dtsi>` path may have changed
2. The `zip_xy_scaler`, `zip_xy_to_scroll_mapper`, `zip_scroll_transform` node names may have been renamed
3. The `INPUT_TRANSFORM_Y_INVERT` constant from `<dt-bindings/zmk/input_transform.h>` may have changed
4. Layer-scoped input processor syntax may have been updated

**The fix:** Verify each include path and node name against current ZMK main source tree.

### SF-3: ZMK Studio Snippet and Configuration

| Attribute | Detail |
|-----------|--------|
| **Confidence** | LOW |
| **Evidence** | `build.yaml` line 20: `snippet: studio-rpc-usb-uart`, line 21: `cmake-args: -DCONFIG_ZMK_STUDIO=y` |
| **Symptom** | ZMK Studio connection failure |
| **Complexity** | Low |

**What changed:** ZMK Studio is a relatively new feature. The snippet mechanism and Studio configuration may have evolved. The `studio-rpc-usb-uart` snippet name and `CONFIG_ZMK_STUDIO` Kconfig may have changed.

**Current code also has:** `&studio_unlock` binding in the NAV layer (keymap line 136), and `studio: true` in `charybdis.zmk.yml`.

**The fix:** Verify snippet name and Kconfig against current ZMK main. Studio is still relatively new so API stability is not guaranteed.

### SF-4: Split Keyboard Central/Peripheral Role Configuration

| Attribute | Detail |
|-----------|--------|
| **Confidence** | MEDIUM |
| **Evidence** | `Kconfig.defconfig`: right half is central, left is peripheral |
| **Symptom** | Split halves cannot communicate |
| **Complexity** | Medium |

**What changed:** This configuration has the RIGHT half as BLE central (unusual -- most split keyboards use the left as central). The `CONFIG_ZMK_SPLIT_BLE_ROLE_CENTRAL` Kconfig and the way split roles are defined may have changed in current ZMK main.

**Specific concerns:**
1. The `Kconfig.defconfig` format using `config ZMK_SPLIT_BLE_ROLE_CENTRAL` with `default y` under shield conditionals may need updating
2. ZMK may have changed how central role assignment works with physical layouts
3. The battery level proxying (`CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING/PROXY`) Kconfig symbols may have been renamed

**The fix:** Verify Kconfig symbol names against current ZMK source. Check if the defconfig format is still valid.

### SF-5: `zmk,battery-nrf-vddh` Compatibility

| Attribute | Detail |
|-----------|--------|
| **Confidence** | LOW |
| **Evidence** | `charybdis.dtsi` line 7: `compatible = "zmk,battery-nrf-vddh";` |
| **Symptom** | Battery level reporting failure |
| **Complexity** | Low |

**What changed:** The battery driver compatible string and API may have changed with Zephyr 4.x. The nice!nano v2 uses VDDH for battery measurement, and ZMK provides this driver. If the compatible string changed or the driver was restructured, battery init could fail.

**Risk note:** A battery driver init failure typically does not prevent device enumeration, but on some Zephyr configurations, a failed device init can cascade.

**The fix:** Verify compatible string against current ZMK board definitions for nice_nano_v2.

---

## Nice-to-Fix: Minor Behavioral Differences

### NF-1: BLE Connection Parameter Tuning

| Attribute | Detail |
|-----------|--------|
| **Confidence** | LOW |
| **Evidence** | `charybdis.conf` lines 4-7 |
| **Symptom** | Slightly different BLE latency or reliability |
| **Complexity** | Low |

The BLE tuning parameters are:
```
CONFIG_BT_PERIPHERAL_PREF_MAX_INT=9
CONFIG_BT_PERIPHERAL_PREF_LATENCY=16
CONFIG_BT_BUF_ACL_TX_COUNT=32
CONFIG_BT_L2CAP_TX_BUF_COUNT=32
```

These Zephyr BLE Kconfig symbols may have been renamed or have different valid ranges in Zephyr 4.x. They are unlikely to cause enumeration failure but may cause warnings or suboptimal behavior.

### NF-2: `CONFIG_BT_CTLR_TX_PWR_PLUS_8` Kconfig

| Attribute | Detail |
|-----------|--------|
| **Confidence** | LOW |
| **Evidence** | `charybdis.conf` line 1 |
| **Symptom** | TX power not at expected level |
| **Complexity** | Low |

This Kconfig for setting BLE TX power to +8 dBm may have been renamed or restructured in Zephyr 4.x BLE controller updates.

### NF-3: Behavior `label` Properties Deprecated

| Attribute | Detail |
|-----------|--------|
| **Confidence** | MEDIUM |
| **Evidence** | `charybdis.keymap` lines 76, 84, 94 |
| **Symptom** | Build warnings |
| **Complexity** | Low |

The custom behaviors (`u_mt`, `u_lt`, `Shift_Enter`) all have `label = "..."` properties. ZMK deprecated the `label` property on behaviors. These should be removed for clean builds, but they are unlikely to cause runtime issues.

### NF-4: `tapping_term_ms` vs `tapping-term-ms` Property Name

| Attribute | Detail |
|-----------|--------|
| **Confidence** | MEDIUM |
| **Evidence** | `charybdis.keymap` line 78: `tapping_term_ms` (underscore) vs line 99: `tapping-term-ms` (hyphen) |
| **Symptom** | Behavior may use default tapping term instead of configured value |
| **Complexity** | Low |

Devicetree property names use hyphens, not underscores. The `u_mt` and `u_lt` behaviors use `tapping_term_ms` (underscore) while `Shift_Enter` correctly uses `tapping-term-ms` (hyphen). Zephyr 4.x may be stricter about this. The underscore version may have worked in older Zephyr as an alias but could be rejected now.

**The fix:** Change `tapping_term_ms` to `tapping-term-ms` in both `u_mt` and `u_lt` behavior definitions.

---

## Anti-Features: Do NOT Build

| Anti-Feature | Why Avoid | What to Do Instead |
|--------------|-----------|-------------------|
| New keymap features | This is a migration, not an enhancement | Fix only what is broken by the Zephyr 4.x transition |
| Auto-mouse layer bug fix | Pre-existing bug, out of scope per PROJECT.md | Document as known issue, defer to separate milestone |
| Driver pinning to old ZMK | Defeats the purpose of migration | Pin PMW3610 driver to compatible commit, but keep ZMK on main |
| Custom board definitions | Adds maintenance burden | Use upstream nice_nano_v2 board definition |

---

## Feature Dependencies

```
MF-1 (Board name) ──> Everything else
  The wrong board name means wrong SoC target; nothing works until this is fixed.

MF-1 ──> MF-2 (HWMv2 format)
  Board name format depends on which Zephyr hardware model version ZMK uses.

MF-1 + MF-2 ──> SF-4 (Split roles)
  Split communication depends on correct board/BLE initialization.

SF-1 (PMW3610 driver) ──> SF-2 (Input processors)
  Input processor config only matters if the driver loads.

SF-3 (ZMK Studio) is independent of trackball fixes.
```

---

## Priority Fix Order (MVP for Enumeration)

1. **MF-1: Fix board name in build.yaml** -- Most likely root cause of total enumeration failure. Change `nice_nano` to `nice_nano_v2` (or HWMv2 equivalent).
2. **MF-2: Verify HWMv2 board format** -- Confirm the exact board identifier string.
3. **MF-3: Remove deprecated `label` from kscan** -- Clean devicetree issue.
4. **MF-4: Verify `CONFIG_ZMK_BLE_EXPERIMENTAL_CONN`** -- Remove if no longer valid.
5. **NF-4: Fix `tapping_term_ms` underscore** -- Quick fix, prevents silent behavior regression.

Then for functionality:
6. **SF-1: Pin and verify PMW3610 driver** -- Get trackball working.
7. **SF-2: Verify input processor API** -- Get scroll/mouse layers working.
8. **SF-4: Verify split Kconfig** -- Get both halves communicating.
9. **SF-3: Verify ZMK Studio** -- Get Studio working.

---

## Sources

- Direct codebase inspection of all config files in this repository
- Training data knowledge of ZMK firmware project (zmk.dev) through ~May 2025
- Training data knowledge of Zephyr RTOS 3.x to 4.x migration (zephyrproject.org)
- Knowledge of badjeff/zmk-pmw3610-driver community project

**Confidence caveat:** All findings marked LOW or MEDIUM confidence MUST be verified against current ZMK main branch source code and documentation before implementation. The most critical finding (MF-1, board name) is HIGH confidence based on direct observation of the `build.yaml` discrepancy with the known hardware (nice!nano v2 / nRF52840).
