# Charybdis Mini ZMK Firmware — Zephyr 4.x Migration

## What This Is

ZMK keyboard firmware for a Charybdis Mini split keyboard (nice!nano v2 MCUs), featuring a PMW3610 optical trackball sensor on the right half. The firmware previously worked on the Zephyr 3.x era and needs to be migrated to the current ZMK main branch (Zephyr 4.x) with minimal changes. The goal is full functionality parity — keyboard input, trackball, layers, combos, and BLE split connectivity — as it existed before.

## Core Value

The keyboard must enumerate and fully function (keys, trackball, BLE) on the current ZMK main / Zephyr 4.x toolchain with zero regression from the Zephyr 3.x behavior.

## Requirements

### Validated

- ✓ 6-layer keymap (BASE, SwapCtrlGui, NUM, NAV, FUN, SCROLL) — existing
- ✓ 8 key combos — existing
- ✓ 3 custom hold-tap behaviors — existing
- ✓ PMW3610 trackball via SPI (badjeff community driver) — existing
- ✓ BLE split keyboard (left central, right peripheral) — existing
- ✓ Auto-mouse layer on trackball movement — existing (with known bug)
- ✓ Scroll mode on SCROLL layer — existing
- ✓ ZMK Studio over USB — existing
- ✓ 5 Bluetooth device slots — existing
- ✓ Battery level reporting from peripheral — existing

### Active

- [ ] Firmware enumerates over USB/BLE on Zephyr 4.x (currently: device does nothing after flash)
- [ ] All keys and layers function identically to Zephyr 3.x behavior
- [ ] PMW3610 trackball driver compatible with current ZMK main
- [ ] west.yml dependencies resolve and build cleanly on current ZMK main
- [ ] GitHub Actions CI build succeeds and produces working .uf2 artifacts

### Out of Scope

- New features or keybindings — this is a compatibility migration only
- Fixing the pre-existing auto-mouse/Fun layer bug — known issue, out of scope for this work
- Restructuring or refactoring the keymap — changes must be minimal
- Pinning to older ZMK versions — goal is current main compatibility

## Context

**Hardware:** Charybdis Mini, nice!nano v2 (nRF52840), PMW3610 trackball on right half, Pro Micro GPIO matrix (4 rows × 6 columns per half).

**Previous working state:** Firmware built and functioned correctly when ZMK was pinned to Zephyr 3.x era. A series of recent commits unpinned from versioned releases (`43c1554` — unpinned zephyr, `7601b0e` — removed nice_nano version tag) to track ZMK main, but the resulting firmware no longer enumerates at all.

**Known breaking changes between Zephyr 3.x → 4.x relevant to this codebase:**
- USB/BLE device descriptor and configuration API changes
- PMW3610 driver API evolution (orientation Kconfig → devicetree flags, already partially handled)
- ZMK input processor API changes affecting trackball/auto-mouse behavior
- Shield/board definition format changes in Zephyr 4.x
- west.yml manifest format or dependency resolution changes

**Existing partial migration work:**
- Commented out deprecated `CONFIG_PMW3610_ALT_INVERT_X/Y` (replaced by dtsi flags)
- Updated pmw3610.dtsi to use hardware orientation flags (lines 51-54)
- Unpinned ZMK to track main branch

**External dependency risk:** `zmk-pmw3610-driver` (badjeff) is community-maintained and actively evolving — API compatibility with current ZMK main must be verified.

## Constraints

- **Minimal changes:** Only modify what is necessary for Zephyr 4.x compatibility — do not refactor or improve beyond what's needed
- **Hardware parity:** Must preserve exact GPIO pin assignments and SPI configuration (hardware cannot change)
- **Build system:** Must continue to build via GitHub Actions using ZMK shared workflow
- **No new dependencies:** Do not introduce new west.yml modules beyond what's already used

## Key Decisions

| Decision | Rationale | Outcome |
|----------|-----------|---------|
| Unpin ZMK to track main | Match current ZMK ecosystem state | ⚠️ Revisit — caused device to stop working |
| Remove nice_nano version tag | Align with current board definitions | — Pending — may need to be investigated |
| Use badjeff PMW3610 driver | Only community driver available for this sensor | — Pending — compatibility with current ZMK main unverified |

---
*Last updated: 2026-03-18 after initialization*
