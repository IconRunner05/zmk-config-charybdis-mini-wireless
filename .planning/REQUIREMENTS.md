# Requirements: Charybdis Mini ZMK — Zephyr 4.x Migration

**Defined:** 2026-03-18
**Core Value:** The keyboard must enumerate and fully function (keys, trackball, BLE) on the current ZMK main / Zephyr 4.x toolchain with zero regression from Zephyr 3.x behavior.

## v1 Requirements

### Build Infrastructure

- [ ] **BUILD-01**: `build.yaml` uses the correct board identifier for nice!nano v2 on current ZMK main (likely `nice_nano_v2`)
- [ ] **BUILD-02**: GitHub Actions CI build completes without errors for both left and right halves
- [ ] **BUILD-03**: `west.yml` resolves all dependencies cleanly against current ZMK main

### Driver Compatibility

- [ ] **DRVR-01**: PMW3610 driver (`badjeff/zmk-pmw3610-driver`) compatible string is verified correct for current driver version
- [ ] **DRVR-02**: PMW3610 Kconfig options (`CONFIG_PMW3610_ALT_*`) verified present and correctly named in current driver
- [ ] **DRVR-03**: PMW3610 devicetree node builds without errors or conflicts

### Devicetree Cleanup

- [ ] **DTS-01**: Deprecated `label = "KSCAN"` property removed from `charybdis.dtsi`
- [ ] **DTS-02**: Manual `#define` input event codes in `pmw3610.dtsi` replaced with proper Zephyr header include (or confirmed not conflicting)
- [ ] **DTS-03**: Any other deprecated Zephyr 4.x devicetree patterns resolved

### Kconfig Audit

- [ ] **KCONF-01**: All Kconfig options in `charybdis.conf` and `charybdis_right.conf` verified valid on current ZMK main (no unknown symbols causing warnings/errors)
- [ ] **KCONF-02**: `CONFIG_ZMK_BLE_EXPERIMENTAL_CONN` status verified (renamed, removed, or still valid)
- [ ] **KCONF-03**: `CONFIG_BT_CTLR_TX_PWR_PLUS_8` status verified on Zephyr 4.x

### Hardware Validation

- [ ] **HW-01**: Device enumerates over USB after flashing (shows up as HID device)
- [ ] **HW-02**: All keys on both halves register correctly
- [ ] **HW-03**: Layer switching works correctly across all 6 layers
- [ ] **HW-04**: Trackball moves mouse cursor on the host
- [ ] **HW-05**: Scroll layer (layer 5) scrolls correctly with trackball
- [ ] **HW-06**: BLE split communication works between left and right halves
- [ ] **HW-07**: Key combos trigger correctly
- [ ] **HW-08**: ZMK Studio accessible over USB

### Stability

- [ ] **STAB-01**: `west.yml` dependencies pinned to known-good commit hashes after successful hardware validation

## v2 Requirements

### Bug Fixes (Pre-existing, deferred)

- **BUG-01**: Auto-mouse layer switching bug — trackball movement incorrectly activates FUN layer instead of auto-mouse layer when SwapCtrlGui is layer 1

## Out of Scope

| Feature | Reason |
|---------|--------|
| New keybindings or layers | Migration only — no feature additions |
| Refactoring keymap structure | Minimal changes only — restructuring risks introducing new bugs |
| Fixing pre-existing auto-mouse bug | Out of scope for this migration work |
| Adding documentation/comments | Out of scope — focus is functional parity |
| Switching to a different PMW3610 driver | Stay with badjeff driver unless it proves incompatible |

## Traceability

| Requirement | Phase | Status |
|-------------|-------|--------|
| BUILD-01 | Phase 1 | Pending |
| BUILD-02 | Phase 1 | Pending |
| BUILD-03 | Phase 1 | Pending |
| DRVR-01 | Phase 2 | Pending |
| DRVR-02 | Phase 2 | Pending |
| DRVR-03 | Phase 2 | Pending |
| DTS-01 | Phase 1 | Pending |
| DTS-02 | Phase 1 | Pending |
| DTS-03 | Phase 1 | Pending |
| KCONF-01 | Phase 1 | Pending |
| KCONF-02 | Phase 1 | Pending |
| KCONF-03 | Phase 1 | Pending |
| HW-01 | Phase 3 | Pending |
| HW-02 | Phase 3 | Pending |
| HW-03 | Phase 3 | Pending |
| HW-04 | Phase 3 | Pending |
| HW-05 | Phase 3 | Pending |
| HW-06 | Phase 3 | Pending |
| HW-07 | Phase 3 | Pending |
| HW-08 | Phase 3 | Pending |
| STAB-01 | Phase 3 | Pending |

**Coverage:**
- v1 requirements: 21 total
- Mapped to phases: 21/21
- Unmapped: 0

---
*Requirements defined: 2026-03-18*
*Last updated: 2026-03-18 after roadmap creation*
