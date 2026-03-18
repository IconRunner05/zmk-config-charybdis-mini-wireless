# Architecture Patterns: ZMK Shield Migration (Zephyr 3.x to 4.x)

**Domain:** ZMK firmware configuration / Zephyr RTOS devicetree migration
**Researched:** 2026-03-18
**Overall confidence:** MEDIUM (based on training data through mid-2025; no live verification of current ZMK main possible -- web search/fetch unavailable)

## Recommended Migration Architecture

The migration requires changes at three distinct levels, each with different scope and risk. The changes are ordered below by dependency -- earlier items must be correct before later items can build on them.

### Change Classification by File

| File | Change Type | Priority | Risk |
|------|-------------|----------|------|
| `build.yaml` | Content change (board name) | 1 - Do first | LOW |
| `charybdis.dtsi` | Structural (remove `label`) | 2 | LOW |
| `Kconfig.defconfig` | Possible structural (split role config) | 3 | MEDIUM |
| `charybdis.conf` | Content (deprecated Kconfig flags) | 4 | MEDIUM |
| `charybdis_right.conf` | Content (Kconfig flags) | 4 | MEDIUM |
| `pmw3610.dtsi` | Content + possible structural | 5 | HIGH |
| `charybdis.keymap` | Content (input processor syntax) | 6 | MEDIUM |
| `west.yml` | Content (driver revision pin) | 7 | HIGH |
| `charybdis_left.overlay` | No change expected | -- | -- |
| `charybdis_right.overlay` | No change expected | -- | -- |
| `charybdis-layouts.dtsi` | No change expected | -- | -- |
| `charybdis.zmk.yml` | No change expected | -- | -- |
| `Kconfig.shield` | No change expected | -- | -- |

## Architectural Changes: Zephyr Level

These are changes driven by the Zephyr RTOS upgrade from 3.x to 4.x that affect ZMK shield configurations.

### Change 1: `label` Property Removal from Devicetree Nodes

**Confidence:** HIGH (well-documented Zephyr deprecation path)

**What changed:** Zephyr deprecated and then removed the `label` property from devicetree nodes. In Zephyr 3.x, nodes like kscan used `label = "KSCAN"` for runtime lookup. Zephyr 4.x uses node references (`&kscan0`) instead of label-based string lookups.

**Impact on this codebase:**

`charybdis.dtsi` line 38 has:
```
label = "KSCAN";
```

This property must be removed. It will either cause a build warning (deprecated) or a build error (if ZMK's devicetree bindings mark it as disallowed).

**File:** `config/boards/shields/charybdis/charybdis.dtsi`
**Action:** Remove `label = "KSCAN";` from the `kscan0` node.

### Change 2: `nice_nano` Board Name Change

**Confidence:** HIGH (commit `7601b0e` already attempted this)

**What changed:** ZMK consolidated board definitions. The versioned `nice_nano@2.0.0` was replaced with just `nice_nano` (unversioned) or `nice_nano_v2` depending on the ZMK version. The current ZMK main branch uses `nice_nano_v2` as the board identifier for the nice!nano v2.

**Impact on this codebase:**

`build.yaml` currently uses `board: nice_nano`. If the current ZMK main expects `nice_nano_v2`, this needs updating. If ZMK main has consolidated to just `nice_nano` mapping to the v2, the current setting works.

**File:** `build.yaml`
**Action:** Verify the correct board identifier. Try `nice_nano_v2` if `nice_nano` fails to resolve. Check ZMK's `app/boards/` directory for the actual board name. The commit history shows progression: `nice_nano@2.0.0` -> `nice_nano` -- but current ZMK main may expect `nice_nano_v2`.

**Confidence note:** MEDIUM. The exact current board name needs verification against the ZMK main branch. This is the most likely cause of "device does nothing after flash" if the board definition fails to match.

### Change 3: SPI Peripheral Naming / SPIM vs SPI

**Confidence:** LOW (needs verification)

**What changed:** Zephyr 4.x may have changed how SPI master nodes are referenced. The `&spi0` reference in `pmw3610.dtsi` depends on the nice!nano board definition providing this node.

**Impact on this codebase:**

`pmw3610.dtsi` line 33 references `&spi0`. If the nice!nano v2 board definition in current ZMK renamed this node (e.g., to `&spi1` or uses a different label), the overlay will silently fail or error.

**File:** `config/boards/shields/charybdis/pmw3610.dtsi`
**Action:** Verify that `&spi0` still exists in the nice!nano v2 board definition on current ZMK main. The `compatible = "nordic,nrf-spim"` is likely still correct.

## Architectural Changes: ZMK Level

These are changes in ZMK's own APIs and configuration system built on top of Zephyr.

### Change 4: Input Processor / Pointing Subsystem Evolution

**Confidence:** MEDIUM

**What changed:** ZMK's pointing subsystem has evolved. The input-listener and input-processor system went through refinements. Key areas:

1. **Input processor includes:** The keymap includes `<input/processors.dtsi>` (line 2) which provides `zip_xy_scaler`, `zip_xy_to_scroll_mapper`, and `zip_scroll_transform`. These are ZMK-provided processor definitions. The include path and available processors may have changed.

2. **`zip_xy_scaler` syntax:** The current usage `<&zip_xy_scaler 1 2>` passes X and Y scale factors as inline arguments. Verify this calling convention hasn't changed.

3. **`zip_scroll_transform` with `INPUT_TRANSFORM_Y_INVERT`:** The `<dt-bindings/zmk/input_transform.h>` header and the `INPUT_TRANSFORM_Y_INVERT` constant should still be valid, but the include path may have changed.

4. **Layer-specific input processor overrides:** The `scroll` child node pattern in `trackball_listener` with `layers = <5>` is the current ZMK convention for per-layer input processing. This pattern should be stable.

**Impact on this codebase:**

`charybdis.keymap` lines 12-27 define the trackball listener:
```
trackball_listener {
    compatible = "zmk,input-listener";
    device = <&trackball>;
    input-processors = <&zip_xy_scaler 1 2>;
    scroll {
        layers = <5>;
        input-processors = ...;
    };
};
```

This structure is likely correct for current ZMK main, as it matches the pattern ZMK standardized on. However, verify:
- That `<input/processors.dtsi>` still exists at that path
- That `zip_xy_scaler`, `zip_xy_to_scroll_mapper`, `zip_scroll_transform` are still the correct processor names
- That child node layer overrides still use `layers` (not some renamed property)

**Files:** `config/charybdis.keymap`
**Action:** If build fails with "unknown node" errors on zip_* processors, check ZMK's current `input/processors.dtsi` for renamed processors or changed include paths.

### Change 5: `CONFIG_ZMK_BLE_EXPERIMENTAL_CONN` Removal

**Confidence:** MEDIUM

**What changed:** ZMK has been graduating experimental BLE connection features to stable defaults. `CONFIG_ZMK_BLE_EXPERIMENTAL_CONN=y` was a flag that enabled improved BLE connection parameters. In newer ZMK, this flag may have been removed (behavior is default) or renamed.

**Impact on this codebase:**

`config/charybdis.conf` line 9: `CONFIG_ZMK_BLE_EXPERIMENTAL_CONN=y`

If this Kconfig option no longer exists, the build will fail with an unknown config warning or error (depending on Kconfig strictness settings).

**File:** `config/charybdis.conf`
**Action:** If build fails mentioning this config, remove the line. The experimental connection behavior is likely now the default.

### Change 6: Split BLE Battery Config Names

**Confidence:** LOW (needs verification)

**What changed:** ZMK may have renamed battery-related Kconfig options as the split battery reporting feature matured.

**Impact on this codebase:**

`config/charybdis.conf` lines 10-11:
```
CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING=y
CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_PROXY=y
```

These may have been renamed or consolidated in current ZMK.

**File:** `config/charybdis.conf`
**Action:** If build fails on these configs, check ZMK's current Kconfig for split battery options.

### Change 7: `compatible = "pixart,pmw3610-alt"` Driver Compatibility

**Confidence:** MEDIUM (critical risk area)

**What changed:** The `badjeff/zmk-pmw3610-driver` is community-maintained and independently evolves. The `compatible` string, Kconfig option names, and devicetree binding properties may have changed. The driver tracks ZMK main, but compatibility windows can be narrow.

**Impact on this codebase:**

`pmw3610.dtsi` uses:
- `compatible = "pixart,pmw3610-alt"` -- may have changed
- `CONFIG_PMW3610_ALT=y` and other `CONFIG_PMW3610_ALT_*` options in `charybdis_right.conf`
- Inline `#define` constants for input event codes (lines 1-12) -- these should use Zephyr headers instead

**Critical observation:** `pmw3610.dtsi` lines 1-12 manually define input event constants:
```c
#define INPUT_EV_KEY 0x01
#define INPUT_EV_REL 0x02
...
```

These should be provided by `<zephyr/dt-bindings/input/input-event-codes.h>` (which is already included in `charybdis_right.overlay`). The manual defines may conflict with or shadow the official header values in Zephyr 4.x if the numeric values changed.

**Files:** `config/boards/shields/charybdis/pmw3610.dtsi`, `config/boards/shields/charybdis/charybdis_right.conf`
**Action:**
1. Replace manual `#define` input event constants in `pmw3610.dtsi` with proper includes from Zephyr headers. The right overlay already includes `<zephyr/dt-bindings/input/input-event-codes.h>` -- move this include to `pmw3610.dtsi` itself and remove the manual defines.
2. Verify the `compatible` string matches what the current driver version expects.
3. Check if `west.yml` should pin the driver to a specific commit known to work with current ZMK main, rather than tracking `main` blindly.

### Change 8: `ZMK_SPLIT_BLE_ROLE_CENTRAL` Kconfig

**Confidence:** MEDIUM

**What changed:** ZMK's split keyboard role configuration has evolved. The `ZMK_SPLIT_BLE_ROLE_CENTRAL` Kconfig flag in `Kconfig.defconfig` determines which half is central. ZMK may have changed the property name or how roles are assigned.

**Impact on this codebase:**

`Kconfig.defconfig` lines 6-7:
```
config ZMK_SPLIT_BLE_ROLE_CENTRAL
    default y
```

This is set under `if SHIELD_CHARYBDIS_RIGHT`, making the right half the central (which connects to the host). This pattern should still work, but verify the config name hasn't changed to something like `ZMK_SPLIT_ROLE_CENTRAL`.

**File:** `config/boards/shields/charybdis/Kconfig.defconfig`
**Action:** Verify config name against current ZMK Kconfig definitions. Likely unchanged but worth confirming.

## Component Boundaries

### What Definitely Does NOT Need Changes

| Component | File(s) | Why Stable |
|-----------|---------|------------|
| Physical layout | `charybdis-layouts.dtsi` | ZMK Studio layout format is relatively new and stable |
| Shield metadata | `charybdis.zmk.yml` | YAML format hasn't changed |
| Shield detection | `Kconfig.shield` | `shields_list_contains` macro is stable Zephyr infrastructure |
| GPIO pin assignments | `*.overlay` col-gpios, row-gpios | Hardware doesn't change; GPIO macros are stable |
| Matrix transforms | `charybdis.dtsi` transforms | `zmk,matrix-transform` compatible is stable |
| Keymap layer definitions | `charybdis.keymap` layers | Key bindings syntax is stable |
| Combo definitions | `charybdis.keymap` combos | `zmk,combos` compatible is stable |
| Custom behaviors | `charybdis.keymap` behaviors | `zmk,behavior-hold-tap` compatible is stable |

### What Likely Needs Changes

| Component | File(s) | Change Type |
|-----------|---------|-------------|
| kscan label | `charybdis.dtsi` | Remove deprecated property |
| Board name | `build.yaml` | Possibly update to `nice_nano_v2` |
| BLE config flags | `charybdis.conf` | Remove/rename deprecated Kconfig |
| Input event defines | `pmw3610.dtsi` | Replace manual defines with header include |
| Driver compatibility | `west.yml` | Possibly pin to known-good revision |

### What MIGHT Need Changes (Investigate at Build Time)

| Component | File(s) | Condition |
|-----------|---------|-----------|
| Input processor syntax | `charybdis.keymap` | Only if ZMK renamed processors |
| SPI node reference | `pmw3610.dtsi` | Only if board definition changed `&spi0` |
| PMW3610 compatible | `pmw3610.dtsi` | Only if driver renamed compatible string |
| Split role config | `Kconfig.defconfig` | Only if ZMK renamed the Kconfig |

## Suggested Migration Order

The changes should be applied in dependency order. Each step should result in a buildable (or at least parseable) state before proceeding.

### Phase 1: Board and Build Infrastructure (get it to compile)

**Goal:** Firmware compiles, even if sensors/BLE may not work yet.

1. **`build.yaml`** -- Verify/update board name (`nice_nano` vs `nice_nano_v2`)
2. **`charybdis.dtsi`** -- Remove `label = "KSCAN"` property
3. **`charybdis.conf`** -- Remove or comment out `CONFIG_ZMK_BLE_EXPERIMENTAL_CONN=y` if it fails
4. **`west.yml`** -- Ensure ZMK `main` and driver `main` are compatible (or pin driver)

**Validation:** `west build` completes without devicetree or Kconfig errors.

### Phase 2: Sensor and Input Pipeline (get trackball working)

**Goal:** PMW3610 driver loads and produces input events.

5. **`pmw3610.dtsi`** -- Replace manual `#define` input constants with Zephyr header include
6. **`charybdis_right.conf`** -- Verify all `CONFIG_PMW3610_ALT_*` options still exist in driver
7. **`west.yml`** -- If driver build fails, pin to a specific known-good commit

**Validation:** Right half builds, trackball produces mouse movement.

### Phase 3: Input Processing and Layers (get scroll and auto-mouse working)

**Goal:** Full input processor pipeline works.

8. **`charybdis.keymap`** -- Verify `<input/processors.dtsi>` include path, processor names
9. **`charybdis.keymap`** -- Verify `zip_scroll_transform` with `INPUT_TRANSFORM_Y_INVERT` works

**Validation:** Scroll layer converts trackball to scroll wheel, all layers functional.

### Phase 4: BLE and Split (get both halves communicating)

**Goal:** Full split keyboard functionality with BLE.

10. **`charybdis.conf`** -- Verify split battery config names
11. **`Kconfig.defconfig`** -- Verify split role config name

**Validation:** Both halves pair, peripheral key presses reach host, battery reporting works.

## Data Flow Changes

### Input Pipeline (Zephyr 3.x vs 4.x)

The high-level data flow has NOT changed architecturally. ZMK's input subsystem still follows:

```
Sensor hardware
  -> Zephyr input subsystem (input events)
    -> ZMK input listener (compatible = "zmk,input-listener")
      -> ZMK input processors (scale, transform, scroll map)
        -> ZMK pointing subsystem
          -> HID report to host
```

The changes are at the binding/configuration level, not the architectural pipeline level. The same `.keymap` structure of `trackball_listener` with child nodes for per-layer overrides is the current canonical pattern.

### Build System Flow

The west manifest flow is unchanged:
```
west.yml
  -> clone zmkfirmware/zmk (main)
  -> clone badjeff/zmk-pmw3610-driver (main)
  -> build with shield overlays and configs
```

The risk is version compatibility between the two `main` branches, not architectural change.

## Anti-Patterns to Avoid

### Anti-Pattern 1: Shotgun Debugging
**What:** Changing many files at once and hoping it compiles.
**Why bad:** Impossible to identify which change fixed which error.
**Instead:** Follow the phased migration order above. Fix one category of errors at a time.

### Anti-Pattern 2: Pinning to Old Driver Commit
**What:** Finding an old `zmk-pmw3610-driver` commit that "works" with current ZMK.
**Why bad:** Creates a fragile state that breaks on the next ZMK update.
**Instead:** Use the driver's `main` branch HEAD or a tagged release. If incompatible, file an issue or find the latest compatible commit and document why.

### Anti-Pattern 3: Keeping Manual Input Event Defines
**What:** Leaving the `#define INPUT_EV_REL 0x02` etc. in `pmw3610.dtsi`.
**Why bad:** These shadow Zephyr's official definitions. If Zephyr 4.x changed any numeric values (unlikely but possible), silent bugs result.
**Instead:** Use `#include <zephyr/dt-bindings/input/input-event-codes.h>` and remove manual defines.

## Sources and Confidence

| Finding | Source | Confidence |
|---------|--------|------------|
| `label` property deprecation/removal | Zephyr 3.x->4.x migration guide (training data) | HIGH |
| nice_nano board name changes | ZMK commit history in this repo | HIGH |
| `CONFIG_ZMK_BLE_EXPERIMENTAL_CONN` graduation | ZMK development patterns (training data) | MEDIUM |
| Input processor syntax stability | ZMK pointing docs (training data, mid-2025) | MEDIUM |
| PMW3610 driver `compatible` string | Codebase analysis + driver evolution (training data) | MEDIUM |
| Manual input event defines as risk | Code review of `pmw3610.dtsi` vs Zephyr headers | HIGH |
| Split battery Kconfig names | Training data | LOW |
| SPI node reference stability | Training data | LOW |

**Critical gap:** Unable to verify current state of ZMK `main` branch or `badjeff/zmk-pmw3610-driver` `main` branch. All ZMK-specific findings should be validated by attempting a build or checking the actual source trees. The phased approach above is designed to surface issues incrementally.

---

*Architecture research: 2026-03-18*
*Confidence limited by inability to access live web sources or run build commands*
