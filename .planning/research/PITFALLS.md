# Domain Pitfalls: ZMK Zephyr 4.x Migration

**Domain:** ZMK split keyboard firmware migration (Zephyr 3.x to 4.x)
**Researched:** 2026-03-18
**Overall Confidence:** MEDIUM (web research tools unavailable; findings based on codebase analysis + training data on ZMK/Zephyr migration patterns; all items marked with confidence levels)

## Critical Pitfalls

These are mistakes that cause the device to not enumerate, not boot, or require significant rework.

### Pitfall 1: `nice_nano` vs `nice_nano_v2` Board Name in build.yaml

**What goes wrong:** The `build.yaml` currently specifies `board: nice_nano` (no version suffix). In Zephyr 4.x / current ZMK main, the nice!nano v2 board definition was renamed or restructured. Using the wrong board identifier causes the build to either fail or produce firmware that does not enumerate because it targets the wrong hardware variant (nRF52832 nice_nano v1 vs nRF52840 nice_nano v2).

**Why it happens:** Commit `7601b0e` removed the version tag from nice_nano, changing from `nice_nano_v2` to `nice_nano`. The original nice_nano (v1) uses nRF52832 with less RAM and no USB, while the v2 uses nRF52840. If the build system resolves `nice_nano` to the v1 definition, the firmware will be compiled for the wrong SoC entirely.

**Consequences:** Firmware does not enumerate at all after flashing. This matches the exact symptom described in the project -- "device does nothing after flash."

**Warning signs:**
- Build log shows nRF52832 instead of nRF52840
- `.uf2` file is significantly smaller than expected
- No USB CDC ACM device appears when connected

**Prevention:**
- Verify the exact board identifier expected by current ZMK main (likely `nice_nano_v2`)
- Check the ZMK `app/boards/` directory in the version being built for the correct board name
- Restore explicit board name in `build.yaml`

**Detection:** Check the build output for the SoC target. Should be nRF52840, not nRF52832.

**Files affected:** `build.yaml` (lines 16, 18, 22)

**Confidence:** MEDIUM -- Training data strongly suggests this is a real issue for nice_nano boards, and the symptom matches. Verify by checking current ZMK main board directory.

**Ordering:** This MUST be investigated first. If the board name is wrong, nothing else matters.

---

### Pitfall 2: Deprecated `label` Property in kscan Node

**What goes wrong:** The `charybdis.dtsi` file includes `label = "KSCAN";` on the kscan node (line 38). In Zephyr 4.x, the `label` property on devicetree nodes was deprecated and then removed. Depending on the Zephyr version, this can cause a devicetree compilation warning or error, or it may cause the kscan device to not be found at runtime.

**Why it happens:** Zephyr 3.x used `label` properties extensively for device lookup. Zephyr 4.x moved to devicetree node references (`DEVICE_DT_GET`). Old configs that still have `label` may silently break device binding.

**Consequences:** If the label causes a DT compilation error, the build fails. If it is silently ignored but the kscan driver expects a different lookup mechanism, keys may not work at all.

**Warning signs:**
- Build warnings about deprecated `label` property
- Build errors in devicetree compilation
- Device boots but no key presses register

**Prevention:**
- Remove `label = "KSCAN";` from the kscan node in `charybdis.dtsi`
- ZMK's own shield definitions no longer use label properties; follow their pattern

**Files affected:** `config/boards/shields/charybdis/charybdis.dtsi` (line 38)

**Confidence:** MEDIUM -- Zephyr 4.x deprecated label properties is well-established in training data. The exact impact on ZMK kscan needs verification.

**Ordering:** Fix after board name issue. This is a build-time or boot-time issue.

---

### Pitfall 3: `CONFIG_PMW3610_ALT` Kconfig Symbol Renamed or Restructured

**What goes wrong:** The right half config uses `CONFIG_PMW3610_ALT=y` and various `CONFIG_PMW3610_ALT_*` settings. The badjeff driver is community-maintained and actively evolving. If the driver's Kconfig symbols changed (e.g., `PMW3610_ALT` renamed to `PMW3610`, or Kconfig options restructured for Zephyr 4.x input subsystem changes), these settings silently become no-ops, and the trackball driver never initializes.

**Why it happens:** Community drivers evolve independently. The driver already went through one rename cycle (CONFIG_PMW3610_INVERT_X/Y to CONFIG_PMW3610_ALT_INVERT_X/Y, then to devicetree properties). Further restructuring for Zephyr 4.x input subsystem is likely.

**Consequences:** SPI bus initializes but PMW3610 driver never loads. Trackball completely non-functional. Keyboard keys may still work.

**Warning signs:**
- Build succeeds but trackball does not respond
- Build warnings about unknown Kconfig symbols (check build log carefully -- these are often buried)
- `CONFIG_PMW3610_ALT` shows as unset in the final `.config`

**Prevention:**
- Pin the `zmk-pmw3610-driver` to a known-working commit hash in `west.yml` instead of tracking `main`
- Before building, check the driver's current `Kconfig` file for the actual symbol names
- After building, grep the build output for "unknown" Kconfig warnings

**Files affected:**
- `config/west.yml` (line 14 -- revision: main)
- `config/boards/shields/charybdis/charybdis_right.conf` (all CONFIG_PMW3610_ALT_* lines)

**Confidence:** MEDIUM -- The pattern of Kconfig renames is documented in the codebase history. Exact current state needs verification against driver HEAD.

**Ordering:** Fix after board name. Requires building against the correct board first to get meaningful results.

---

### Pitfall 4: Split Keyboard Central/Peripheral Role Inversion

**What goes wrong:** In `Kconfig.defconfig`, the RIGHT half is configured as `ZMK_SPLIT_BLE_ROLE_CENTRAL`. ZMK convention is that the central half is the one connected to USB. For this Charybdis, the right half has the trackball (pointing device) and is set as central. This is an intentional design choice for this specific keyboard -- BUT if current ZMK main changed how pointing device data flows in split keyboards, or if the central role assignment interacts differently with Zephyr 4.x BLE stack changes, this could cause enumeration failures.

**Why it happens:** Normally the left half is central in ZMK splits. This config intentionally makes the right half central because the trackball is on the right side and pointing data needs to flow through the central half. If ZMK's split pointing architecture changed (e.g., to support peripheral-side pointing natively), the role assignment may need updating.

**Consequences:** USB enumeration fails if the wrong half is expected to be central. BLE split connection fails if the role negotiation changed.

**Warning signs:**
- Left half connected via USB does not enumerate (because it is the peripheral, not central)
- Right half connected via USB works but left half never pairs
- Build log shows split role configuration warnings

**Prevention:**
- Verify that the USB cable is connected to the RIGHT half (the central)
- Check current ZMK main for any changes to split pointing device support
- If ZMK now supports peripheral-side pointing, the left half could become central (conventional) and pointing data relayed from right peripheral

**Files affected:** `config/boards/shields/charybdis/Kconfig.defconfig` (lines 6-8)

**Confidence:** MEDIUM -- The config is clearly intentional but may conflict with ZMK architecture changes. The "device does nothing after flash" symptom could be caused by connecting USB to the wrong half.

**Ordering:** Verify alongside the board name issue. This is a quick check.

---

### Pitfall 5: Unpinned Dependencies Creating Build Non-Reproducibility

**What goes wrong:** Both `zmk` and `zmk-pmw3610-driver` track `main` in `west.yml`. Every build pulls whatever commit happens to be HEAD at that moment. A breaking change in either repo between builds causes silent failures.

**Why it happens:** The west.yml was intentionally changed to track main (commit `43c1554`), but this means the exact code built is never the same twice.

**Consequences:**
- Build that worked yesterday fails today
- Firmware behavior changes between builds with no local changes
- Extremely difficult to debug because the upstream code is a moving target

**Warning signs:**
- CI builds fail intermittently
- Same config produces different behavior on different build dates
- West update pulls unexpected changes

**Prevention:**
- Pin `zmk` to a specific commit hash known to work (e.g., a recent ZMK main commit)
- Pin `zmk-pmw3610-driver` to a specific commit hash
- Once working, record the exact commit hashes that produced a working build
- Only update pins deliberately, testing after each update

**Files affected:** `config/west.yml` (lines 10, 14)

**Confidence:** HIGH -- This is a well-known best practice and the codebase history shows it was previously pinned.

**Ordering:** Pin dependencies BEFORE debugging other issues. Without pinning, the target keeps moving.

---

### Pitfall 6: `compatible = "pixart,pmw3610-alt"` String May Have Changed

**What goes wrong:** The devicetree compatible string `"pixart,pmw3610-alt"` in `pmw3610.dtsi` must exactly match what the driver declares. If the badjeff driver renamed its compatible string (e.g., to `"pixart,pmw3610"` without the `-alt` suffix, or added a vendor prefix change), the devicetree binding fails silently and the driver never loads.

**Why it happens:** The `-alt` suffix suggests this was an alternative/fork version. As the driver matures and becomes the de facto standard, the compatible string may be simplified.

**Consequences:** Build succeeds but trackball hardware is never bound to a driver. Complete trackball failure at runtime.

**Warning signs:**
- Build warning: "no binding for compatible 'pixart,pmw3610-alt'"
- Trackball completely unresponsive despite SPI being configured
- No devicetree binding YAML file matches the compatible string

**Prevention:**
- Check the driver's current `dts/bindings/` directory for the YAML binding file
- The compatible string in that YAML must match what `pmw3610.dtsi` uses
- Search the driver repo for `compatible =` in binding YAML files

**Files affected:** `config/boards/shields/charybdis/pmw3610.dtsi` (line 43)

**Confidence:** MEDIUM -- The pattern is sound but the specific rename status needs verification.

**Ordering:** Check alongside Pitfall 3 (Kconfig symbols). Both relate to driver compatibility.

---

## Moderate Pitfalls

### Pitfall 7: `CONFIG_ZMK_BLE_EXPERIMENTAL_CONN` Removed or Renamed

**What goes wrong:** The `charybdis.conf` uses `CONFIG_ZMK_BLE_EXPERIMENTAL_CONN=y`. If this experimental feature was graduated to stable (renamed or removed as a separate option), the Kconfig symbol becomes unknown. This may cause build warnings or, worse, the BLE connection parameters it enabled are no longer applied, leading to degraded BLE performance or split connection instability.

**Prevention:**
- Check current ZMK Kconfig for this symbol
- If removed, the feature may now be default behavior (no config needed)
- If renamed, update the config

**Files affected:** `config/charybdis.conf` (line 9)

**Confidence:** MEDIUM -- Experimental features in ZMK frequently graduate or get renamed.

---

### Pitfall 8: BLE Peripheral Preferred Connection Parameters May Be Ignored

**What goes wrong:** `charybdis.conf` sets several `CONFIG_BT_PERIPHERAL_PREF_*` and `CONFIG_BT_BUF_*` values. Zephyr 4.x restructured the Bluetooth subsystem Kconfig. These symbols may have been renamed, moved under different Kconfig menus, or have different valid ranges.

**Prevention:**
- Verify each BT config symbol exists in current Zephyr Kconfig
- Check for build warnings about unknown symbols
- BLE connection issues (lag, disconnects) after migration indicate these settings are not being applied

**Files affected:** `config/charybdis.conf` (lines 4-7)

**Confidence:** LOW -- Zephyr BT Kconfig does evolve but specific renames are uncertain without checking current source.

---

### Pitfall 9: `#include <input/processors.dtsi>` Path Changed

**What goes wrong:** The keymap includes `<input/processors.dtsi>` which provides `zip_xy_scaler`, `zip_xy_to_scroll_mapper`, and `zip_scroll_transform`. If ZMK moved or renamed this include file, the build fails with a missing include error.

**Prevention:**
- This is a build-time error and will be immediately obvious
- Check ZMK's `app/dts/` directory for the current include path
- May have moved to `<zmk/input/processors.dtsi>` or similar

**Files affected:** `config/charybdis.keymap` (line 2)

**Confidence:** MEDIUM -- ZMK has been reorganizing DTS includes.

---

### Pitfall 10: `snippet: studio-rpc-usb-uart` May Not Exist in Current ZMK

**What goes wrong:** The `build.yaml` for the right half includes `snippet: studio-rpc-usb-uart`. ZMK snippets are a relatively new feature and the naming/availability may have changed. If the snippet does not exist, the build fails or ZMK Studio support silently breaks.

**Prevention:**
- Verify the snippet name in current ZMK main
- Check `app/snippets/` in the ZMK repo for available snippets
- If the snippet was renamed, update `build.yaml`

**Files affected:** `build.yaml` (line 20)

**Confidence:** LOW -- Snippet system is newer and may be stable, but worth verifying.

---

### Pitfall 11: Hardcoded Input Event Code Defines in pmw3610.dtsi

**What goes wrong:** The `pmw3610.dtsi` file manually defines input event codes (`INPUT_EV_REL`, `INPUT_REL_X`, etc.) at the top of the file (lines 1-12) instead of including them from Zephyr headers. If the actual values changed in Zephyr 4.x (unlikely but possible), or if the driver now expects the defines to come from the standard Zephyr header, there could be a mismatch.

**Why it happens:** The right overlay (`charybdis_right.overlay`) does include `<zephyr/dt-bindings/input/input-event-codes.h>`, but `pmw3610.dtsi` redefines them locally. If both are included, there could be redefinition warnings or conflicts.

**Prevention:**
- Replace the manual `#define` block in `pmw3610.dtsi` with `#include <zephyr/dt-bindings/input/input-event-codes.h>`
- Or ensure values match the Zephyr 4.x header values (they are likely the same, as these are Linux-standard values)

**Files affected:** `config/boards/shields/charybdis/pmw3610.dtsi` (lines 1-12)

**Confidence:** LOW -- Values are Linux standard and unlikely to change, but the redefinition pattern is fragile.

---

### Pitfall 12: `CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING` Renamed

**What goes wrong:** Battery level fetching/proxy settings in `charybdis.conf` may have been restructured. ZMK has been refactoring split BLE internals.

**Prevention:**
- Verify Kconfig symbol names against current ZMK source
- Check for build warnings about unknown configuration

**Files affected:** `config/charybdis.conf` (lines 10-11)

**Confidence:** LOW -- These are established ZMK features but naming may evolve.

---

## Minor Pitfalls

### Pitfall 13: GitHub Actions Workflow Reference `@main` Is Also Unpinned

**What goes wrong:** The build workflow (`build.yml`) references `zmkfirmware/zmk/.github/workflows/build-user-config.yml@main`. This means the build workflow itself can change between runs. If ZMK changes the shared workflow inputs (e.g., requiring new parameters, changing artifact naming), builds break with no local changes.

**Prevention:**
- Pin to a specific commit or tag: `@<commit-sha>` or `@<tag>`
- Or accept the risk and monitor CI builds

**Files affected:** `.github/workflows/build.yml` (line 5)

**Confidence:** HIGH -- This is a well-understood CI/CD pattern.

---

### Pitfall 14: Physical Layout DTI Changes

**What goes wrong:** The `charybdis-layouts.dtsi` uses `#include <physical_layouts.dtsi>` and `zmk,physical-layout` compatible. If ZMK restructured how physical layouts are defined (this is a newer ZMK feature for Studio support), the format may need updating.

**Prevention:**
- Check current ZMK examples for physical layout format
- This will manifest as a build error (straightforward to debug)

**Files affected:** `config/boards/shields/charybdis/charybdis-layouts.dtsi`

**Confidence:** LOW -- Physical layouts are a newer ZMK feature that is likely still evolving.

---

### Pitfall 15: `tapping_term_ms` vs `tapping-term-ms` Property Name

**What goes wrong:** The `u_mt` behavior in the keymap uses `tapping_term_ms` (with underscores, line 78) while `Shift_Enter` uses `tapping-term-ms` (with hyphens, line 99). In devicetree, property names use hyphens. The underscore variant may have been accepted in older Zephyr DTS parsers but could be rejected or silently ignored in Zephyr 4.x.

**Why it happens:** Devicetree convention uses hyphens. Some older ZMK examples used underscores. The DTS compiler may or may not convert between them.

**Consequences:** If the underscore variant is silently ignored, the hold-tap behavior uses its default tapping term instead of 200ms, changing the feel of the keyboard.

**Warning signs:**
- Hold-tap behavior feels different (triggers hold too easily or too slowly)
- DTS compilation warnings about unknown properties

**Prevention:**
- Normalize all property names to use hyphens: `tapping-term-ms`
- Check both `u_mt` and `u_lt` behaviors

**Files affected:** `config/charybdis.keymap` (lines 78, 86)

**Confidence:** MEDIUM -- Devicetree standard uses hyphens; Zephyr DTS compiler behavior for underscores may vary by version.

---

## Phase-Specific Warnings

| Phase/Task | Likely Pitfall | Mitigation | Priority |
|------------|---------------|------------|----------|
| Fix board name | Pitfall 1: nice_nano vs nice_nano_v2 | Check ZMK board directory for correct name | **P0 -- do first** |
| Pin dependencies | Pitfall 5: unpinned west.yml | Pin both zmk and pmw3610-driver to specific commits | **P0 -- do first** |
| Fix devicetree | Pitfall 2: deprecated label property | Remove `label = "KSCAN"` | P1 |
| Fix devicetree | Pitfall 15: underscore vs hyphen | Normalize to hyphens | P1 |
| Fix devicetree | Pitfall 11: hardcoded input event defines | Use Zephyr header include | P1 |
| Verify driver compat | Pitfall 3: Kconfig symbols renamed | Check driver Kconfig file | P1 |
| Verify driver compat | Pitfall 6: compatible string changed | Check driver binding YAML | P1 |
| Verify BLE config | Pitfall 7: experimental conn renamed | Check ZMK Kconfig | P2 |
| Verify BLE config | Pitfall 8: BT peripheral params | Check Zephyr BT Kconfig | P2 |
| Verify build system | Pitfall 10: snippet name | Check ZMK snippets dir | P2 |
| Verify build system | Pitfall 13: workflow ref unpinned | Pin workflow ref | P3 |
| Verify keymap includes | Pitfall 9: input processors path | Check ZMK DTS includes | P1 |
| Verify split roles | Pitfall 4: central/peripheral assignment | Confirm USB connected to right half | P0 -- quick check |

## Recommended Investigation Order

1. **Board name** (Pitfall 1) -- Most likely cause of "device does nothing." Quick to verify and fix.
2. **Pin dependencies** (Pitfall 5) -- Ensures reproducible builds before debugging anything else.
3. **Driver compatibility** (Pitfalls 3, 6) -- Check Kconfig symbols and compatible string against actual driver source.
4. **Devicetree fixes** (Pitfalls 2, 11, 15) -- Clean up known deprecated patterns.
5. **Verify keymap includes** (Pitfall 9) -- Ensure DTS include paths are current.
6. **BLE and split config** (Pitfalls 4, 7, 8) -- Verify once basic enumeration works.
7. **Build system** (Pitfalls 10, 13, 14) -- Lower priority; build failures are obvious.

## Key Insight: The "Device Does Nothing" Symptom

The reported symptom -- device does not enumerate at all after flashing -- strongly points to either:

1. **Wrong board target** (Pitfall 1): Firmware compiled for nRF52832 flashed to nRF52840 hardware. This is the most likely root cause given that commit `7601b0e` explicitly changed the board name.

2. **USB connected to wrong half** (Pitfall 4): If USB is plugged into the left (peripheral) half, it will not enumerate because only the central (right) half handles USB in this config.

3. **Boot crash from incompatible devicetree** (Pitfalls 2, 3, 6): If the firmware compiles but has devicetree errors that cause a crash during device initialization, the USB stack never starts.

These should be investigated in this order as they go from simplest to most complex.

## Sources

- Codebase analysis of all config files in this repository
- ZMK documentation patterns (training data, MEDIUM confidence)
- Zephyr 4.x migration knowledge (training data, MEDIUM confidence)
- Git commit history analysis from PROJECT.md and CONCERNS.md

**NOTE:** Web research tools were unavailable during this research session. All findings beyond direct codebase analysis are based on training data (knowledge cutoff ~May 2025). Specific Kconfig symbol names, compatible strings, and board identifiers MUST be verified against the current ZMK main and badjeff driver HEAD before acting on them. Confidence levels reflect this limitation.

---

*Pitfalls audit: 2026-03-18*
