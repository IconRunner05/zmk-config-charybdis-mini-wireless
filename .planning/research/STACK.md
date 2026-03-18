# Technology Stack: ZMK Zephyr 4.x Migration

**Project:** Charybdis Mini ZMK Firmware Migration
**Researched:** 2026-03-18
**Overall Confidence:** MEDIUM (training data only -- WebSearch, WebFetch, and GitHub API were unavailable; all findings need validation against current ZMK main)

## Critical Limitation

This research is based entirely on training data (cutoff ~May 2025) and analysis of the local codebase. I was unable to verify any claims against live sources (ZMK GitHub, Zephyr changelogs, badjeff driver repo). Every finding below should be validated before implementation. Confidence levels reflect this constraint.

---

## Recommended Stack Changes

### 1. Board Name: `nice_nano` vs `nice_nano_v2` (MEDIUM confidence)

| Item | Current Value | Required Change | Rationale |
|------|--------------|-----------------|-----------|
| `build.yaml` board | `nice_nano` | Change to `nice_nano_v2` | Zephyr 4.x uses hardware revision-qualified board names via the new board model |

**Details:**

Zephyr introduced a new "hardware model v2" board naming system starting around Zephyr 3.7/4.0. Under this model, boards with hardware revisions are referenced using their full qualified name. The nice!nano v2 hardware should be referenced as `nice_nano_v2` (not `nice_nano`) in current ZMK main.

The commit `7601b0e` ("Updated build.yaml to remove version tag from nice_nano") removed the version qualifier, which is the opposite of what current ZMK main expects. This is a likely contributor to the "device does nothing after flash" problem -- it may be building against the wrong board definition entirely, or failing to find a valid board definition.

**Before (build.yaml):**
```yaml
include:
  - board: nice_nano
    shield: charybdis_left
```

**After (build.yaml):**
```yaml
include:
  - board: nice_nano_v2
    shield: charybdis_left
```

**Validation required:** Check what board names exist in the current ZMK main `app/boards/` directory. The exact name may be `nice_nano_v2`, `nice_nano/v2`, or use Zephyr's new `board@revision` syntax. Run `ls` on a west workspace or check ZMK's boards directory on GitHub.

### 2. West Manifest (west.yml) (HIGH confidence -- verified against local file)

| Item | Current Value | Required Change | Rationale |
|------|--------------|-----------------|-----------|
| ZMK revision | `main` | Keep as `main` (or pin to known-good commit) | Tracking main is the goal per PROJECT.md |
| PMW3610 driver revision | `main` | Pin to a specific known-good commit hash | Community driver evolves independently; `main` may break at any time |

**Current west.yml is structurally valid** for the `zmk-config` approach. The manifest format itself has not changed between Zephyr 3.x and 4.x for user configs. The `import: app/west.yml` pattern remains the standard way for user configs to pull in ZMK's dependencies.

**Recommendation:** After the migration builds successfully, pin both `zmk` and `zmk-pmw3610-driver` to specific commit SHAs:

```yaml
manifest:
  remotes:
    - name: zmkfirmware
      url-base: https://github.com/zmkfirmware
    - name: badjeff
      url-base: https://github.com/badjeff
  projects:
    - name: zmk
      remote: zmkfirmware
      revision: main  # Pin to commit SHA after successful build
      import: app/west.yml
    - name: zmk-pmw3610-driver
      remote: badjeff
      revision: main  # Pin to commit SHA after successful build
  self:
    path: config
```

### 3. Kconfig Changes (MEDIUM confidence)

#### 3a. Likely Valid -- No Changes Needed

These Kconfig options from `charybdis.conf` are standard ZMK options that should still work on current ZMK main:

| Option | Status | Confidence |
|--------|--------|------------|
| `CONFIG_ZMK_POINTING=y` | Still valid | HIGH |
| `CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING=y` | Still valid | MEDIUM |
| `CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_PROXY=y` | Still valid | MEDIUM |
| `CONFIG_ZMK_BEHAVIORS_QUEUE_SIZE=512` | Still valid | HIGH |
| `CONFIG_BT_BUF_ACL_TX_COUNT=32` | Still valid | HIGH |
| `CONFIG_BT_L2CAP_TX_BUF_COUNT=32` | Still valid | HIGH |
| `CONFIG_BT_PERIPHERAL_PREF_MAX_INT=9` | Still valid | HIGH |
| `CONFIG_BT_PERIPHERAL_PREF_LATENCY=16` | Still valid | HIGH |

#### 3b. Potentially Renamed or Removed

| Option | Current Value | Possible Change | Confidence |
|--------|--------------|-----------------|------------|
| `CONFIG_ZMK_BLE_EXPERIMENTAL_CONN=y` | In charybdis.conf | May have been promoted to default or renamed. ZMK periodically graduates experimental features. | LOW |
| `CONFIG_BT_CTLR_TX_PWR_PLUS_8=y` | In charybdis.conf | Zephyr 4.x may have changed the BLE controller power Kconfig namespace. Check for `CONFIG_BT_CTLR_TX_PWR` integer option replacing the `_PLUS_8` boolean. | LOW |

#### 3c. PMW3610 Driver Kconfig (charybdis_right.conf)

| Option | Status | Notes |
|--------|--------|-------|
| `CONFIG_SPI=y` | Still required | Core Zephyr, unchanged |
| `CONFIG_INPUT=y` | Still required | Core Zephyr input subsystem |
| `CONFIG_ZMK_POINTING=y` | Duplicate of charybdis.conf | Harmless but redundant |
| `CONFIG_ZMK_EXT_POWER=y` | Still valid | MEDIUM confidence |
| `CONFIG_PMW3610_ALT=y` | **Needs verification** | The badjeff driver has been through multiple naming iterations. May now be `CONFIG_PMW3610` without the `_ALT` suffix, or may remain as-is. |
| `CONFIG_PMW3610_ALT_RUN_DOWNSHIFT_TIME_MS=3264` | **Needs verification** | May have been renamed if `_ALT` suffix was dropped |
| `CONFIG_PMW3610_ALT_REST1_SAMPLE_TIME_MS=40` | **Needs verification** | Same concern |
| `CONFIG_PMW3610_ALT_SMART_ALGORITHM=y` | **Needs verification** | Same concern |
| `CONFIG_PMW3610_ALT_INIT_POWER_UP_EXTRA_DELAY_MS=1000` | **Needs verification** | Same concern |

**Key risk:** The badjeff `zmk-pmw3610-driver` has undergone significant API churn. The `_ALT` suffix in all config names suggests this codebase was already migrated once from the original PMW3610 driver to the "alt" fork. The driver's main branch may have undergone further renames.

#### 3d. Potentially New Required Kconfig Options

| Option | Purpose | When Needed | Confidence |
|--------|---------|-------------|------------|
| `CONFIG_ZMK_SPLIT_BLE_PREF_WEAK_BOND=y` | Improved split reconnection | If split halves have trouble reconnecting after Zephyr 4.x migration | LOW |
| `CONFIG_CLOCK_CONTROL=y` | Required by some Zephyr 4.x drivers | May be needed if SPI/trackball fails to init | LOW |

### 4. Devicetree Changes (MEDIUM confidence)

#### 4a. `label` Property Deprecation

Zephyr 4.x deprecated the `label` property on devicetree nodes in favor of node names for device identification. The current codebase has:

**charybdis.dtsi line 39:**
```dts
label = "KSCAN";
```

**Behaviors in charybdis.keymap lines 76, 86, 94:**
```dts
label = "u_mt";
label = "u_lt";
label = "SHIFT_ENTER";
```

**Required change:** Remove `label` properties. In Zephyr 4.x, devices are identified by their node path, not by label strings. ZMK's current main branch should handle this, but having stale `label` properties may cause warnings or (less likely) build errors.

**Confidence:** MEDIUM -- the deprecation is well-documented in Zephyr, but ZMK may still accept `label` for backward compatibility.

#### 4b. `compatible` for PMW3610

**Current (`pmw3610.dtsi`):**
```dts
compatible = "pixart,pmw3610-alt";
```

**Possible change:** The compatible string may have changed in the badjeff driver. Verify against the current driver's `dts/bindings/` directory. If the driver dropped the `-alt` suffix, this would need to change to `"pixart,pmw3610"`.

**Confidence:** LOW -- cannot verify without checking the driver repo.

#### 4c. Input Event Code Defines

**Current (`pmw3610.dtsi` lines 1-12):**
The file manually defines `INPUT_EV_*` and `INPUT_REL_*` constants.

**Possible issue:** In Zephyr 4.x, these are provided by `<zephyr/dt-bindings/input/input-event-codes.h>`. The manual defines may conflict. The `charybdis_right.overlay` already includes `<zephyr/dt-bindings/input/input-event-codes.h>` (line 9), so having manual defines in `pmw3610.dtsi` could cause redefinition warnings or errors.

**Recommended change:** Replace the manual `#define` block in `pmw3610.dtsi` with:
```dts
#include <zephyr/dt-bindings/input/input-event-codes.h>
```

**Confidence:** MEDIUM -- the header has existed since Zephyr 3.4+, and manual defines will at minimum cause warnings.

#### 4d. SPI Node Changes

The `&spi0` node configuration in `pmw3610.dtsi` should still work, but verify that:
- The `compatible = "nordic,nrf-spim"` is still the correct compatible for nRF52840 SPI in Zephyr 4.x
- The `pinctrl` approach (using `pinctrl-0`, `pinctrl-1`, `pinctrl-names`) remains unchanged (it should -- this is the standard Zephyr pinctrl model)

**Confidence:** HIGH that pinctrl approach is unchanged. MEDIUM that the compatible string is unchanged.

### 5. Build System / GitHub Actions (HIGH confidence)

| Item | Current | Change Needed? | Notes |
|------|---------|----------------|-------|
| Shared workflow ref | `@main` | No change needed | Already tracking ZMK main |
| `snippet` field | `studio-rpc-usb-uart` | Verify snippet name | Snippets are a newer ZMK feature; name should be stable |
| `cmake-args` | `-DCONFIG_ZMK_STUDIO=y` | No change needed | Standard cmake override |

### 6. Keymap / Shield Structure (HIGH confidence)

The keymap file structure (`charybdis.keymap`) and shield definition files (`Kconfig.shield`, `Kconfig.defconfig`, `charybdis.zmk.yml`) should not require changes for the Zephyr 4.x migration. These are ZMK-layer abstractions that ZMK maintains backward compatibility for.

The `charybdis.zmk.yml` with `file_format: "1"` is the current metadata format.

---

## Priority-Ordered Migration Checklist

Based on the analysis, here is the most likely path to a working build, ordered by probability of being the root cause:

### Priority 1: Board Name (Most Likely Root Cause)

Change `nice_nano` to the correct board identifier in `build.yaml`. This is the most probable cause of "device does nothing after flash" -- building against a wrong/missing board definition would produce firmware that cannot boot.

### Priority 2: PMW3610 Driver Compatibility

Verify the `zmk-pmw3610-driver` `main` branch builds against current ZMK `main`. Check:
- Compatible string (`pixart,pmw3610-alt` vs `pixart,pmw3610`)
- Kconfig prefix (`CONFIG_PMW3610_ALT_*` vs `CONFIG_PMW3610_*`)
- Any new required devicetree properties

### Priority 3: Input Event Code Conflicts

Remove manual `#define` blocks from `pmw3610.dtsi` and use the Zephyr header include instead.

### Priority 4: Deprecated `label` Properties

Remove `label` properties from devicetree nodes if they cause build errors (may just be warnings).

### Priority 5: Kconfig Renames

Check `CONFIG_BT_CTLR_TX_PWR_PLUS_8` and `CONFIG_ZMK_BLE_EXPERIMENTAL_CONN` for renames.

---

## Alternatives Considered

| Decision | Chosen | Alternative | Why Not |
|----------|--------|-------------|---------|
| Track ZMK main | Yes (per project requirements) | Pin to last known Zephyr 3.x ZMK release | Project goal is Zephyr 4.x compatibility |
| Use badjeff PMW3610 driver | Yes (only option) | Write custom driver | No other community driver exists for PMW3610 on ZMK |
| nice_nano_v2 board | Recommended | nice_nano without revision | Zephyr 4.x hw model requires revision-qualified names |

---

## Verification Steps (Cannot Be Skipped)

Since this research was conducted without access to live sources, the following MUST be verified before making changes:

1. **Board name:** Check `zmkfirmware/zmk` repo `app/boards/` for the exact nice!nano board directory/name structure
2. **PMW3610 driver:** Check `badjeff/zmk-pmw3610-driver` repo for:
   - Current compatible string in `dts/bindings/`
   - Current Kconfig symbol names in `Kconfig`
   - Any migration guide or breaking change notes in README/commits
3. **ZMK Zephyr version:** Check `zmk/app/west.yml` to confirm which Zephyr version ZMK main currently uses (expected: Zephyr 4.0 or 4.1)
4. **Kconfig renames:** Run a test build and check for "unknown configuration option" warnings in the build log
5. **BT controller TX power:** Check if `CONFIG_BT_CTLR_TX_PWR_PLUS_8` still exists in Zephyr's Kconfig or was replaced

---

## Sources

| Source | Type | Confidence |
|--------|------|------------|
| Local codebase analysis | Direct inspection | HIGH |
| Zephyr board model v2 changes | Training data (2024-2025 Zephyr changelogs) | MEDIUM |
| ZMK board naming conventions | Training data (ZMK Discord, docs) | MEDIUM |
| PMW3610 driver API evolution | Training data (GitHub issues/PRs) | LOW |
| Kconfig deprecations | Training data (Zephyr migration guides) | MEDIUM |
| Devicetree label deprecation | Training data (Zephyr 3.5+ release notes) | MEDIUM |

**Critical gap:** No live verification was possible. All MEDIUM and LOW confidence items should be checked against current GitHub repos before implementation.

---

*Stack research: 2026-03-18*
