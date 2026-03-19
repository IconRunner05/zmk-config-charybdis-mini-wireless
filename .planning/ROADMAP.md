# Roadmap: Charybdis Mini ZMK — Zephyr 4.x Migration

## Overview

This migration takes a non-functional ZMK firmware (builds but device does nothing after flash) and systematically restores full functionality on the current ZMK main / Zephyr 4.x toolchain. The work moves from getting a clean build, to ensuring the community trackball driver is compatible, to verifying every feature works on real hardware and locking down dependencies.

## Phases

**Phase Numbering:**
- Integer phases (1, 2, 3): Planned milestone work
- Decimal phases (2.1, 2.2): Urgent insertions (marked with INSERTED)

- [ ] **Phase 1: Build Fix and Cleanup** - Restore a clean, warning-free CI build for both halves on current ZMK main
- [ ] **Phase 2: Driver Compatibility** - Verify and fix PMW3610 trackball driver for current ZMK main
- [ ] **Phase 3: Hardware Validation and Stabilization** - Confirm full functionality on hardware and pin dependencies

## Phase Details

### Phase 1: Build Fix and Cleanup
**Goal**: Firmware compiles cleanly for both halves with zero errors or unresolved symbols on current ZMK main
**Depends on**: Nothing (first phase)
**Requirements**: BUILD-01, BUILD-02, BUILD-03, DTS-01, DTS-02, DTS-03, KCONF-01, KCONF-02, KCONF-03
**Success Criteria** (what must be TRUE):
  1. `build.yaml` specifies the correct board identifier and GitHub Actions CI produces .uf2 artifacts for both left and right halves without errors
  2. `west.yml` resolves all module dependencies on first build with no fetch failures or version conflicts
  3. All devicetree files compile without deprecation warnings or redefinition conflicts
  4. All Kconfig symbols resolve to valid options with no "unknown symbol" warnings in the build log
**Plans**: TBD

Plans:
- [ ] 01-01: TBD
- [ ] 01-02: TBD

### Phase 2: Driver Compatibility
**Goal**: PMW3610 trackball driver integrates cleanly with the firmware build and current ZMK input processor API
**Depends on**: Phase 1
**Requirements**: DRVR-01, DRVR-02, DRVR-03
**Success Criteria** (what must be TRUE):
  1. The PMW3610 compatible string in the devicetree matches the current badjeff driver version exactly
  2. All PMW3610 Kconfig options used in conf files exist in the current driver and are correctly named
  3. The PMW3610 devicetree node compiles without errors alongside the rest of the shield definition
**Plans**: TBD

Plans:
- [ ] 02-01: TBD

### Phase 3: Hardware Validation and Stabilization
**Goal**: The keyboard fully functions on real hardware with zero regression from Zephyr 3.x behavior, and dependencies are pinned to prevent future breakage
**Depends on**: Phase 2
**Requirements**: HW-01, HW-02, HW-03, HW-04, HW-05, HW-06, HW-07, HW-08, STAB-01
**Success Criteria** (what must be TRUE):
  1. Device enumerates as a HID device over USB and all keys on both halves register correctly
  2. Trackball moves the mouse cursor and scroll layer scrolls correctly
  3. BLE split communication works between halves, all 6 layers switch correctly, and combos trigger
  4. ZMK Studio is accessible over USB
  5. `west.yml` dependencies are pinned to known-good commit hashes after successful validation
**Plans**: TBD

Plans:
- [ ] 03-01: TBD

## Progress

**Execution Order:**
Phases execute in numeric order: 1 → 2 → 3

| Phase | Plans Complete | Status | Completed |
|-------|----------------|--------|-----------|
| 1. Build Fix and Cleanup | 0/? | Not started | - |
| 2. Driver Compatibility | 0/? | Not started | - |
| 3. Hardware Validation and Stabilization | 0/? | Not started | - |
