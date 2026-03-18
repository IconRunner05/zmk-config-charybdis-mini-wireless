# Research Summary: ZMK Zephyr 4.x Migration

**Domain:** Embedded firmware migration (ZMK keyboard firmware, Zephyr RTOS)
**Researched:** 2026-03-18
**Overall confidence:** MEDIUM (no live source verification was possible)

## Executive Summary

This research investigates the required changes to migrate a Charybdis Mini ZMK firmware config from Zephyr 3.x to Zephyr 4.x (current ZMK main branch). The firmware currently builds but produces non-functional output ("device does nothing after flash").

The most probable root cause is the board name change in `build.yaml`. Commit `7601b0e` removed the version tag from `nice_nano`, changing it from `nice_nano_v2` to `nice_nano`. However, Zephyr 4.x introduced a new hardware model that requires revision-qualified board names, meaning the build is likely targeting the wrong board definition or failing silently. Restoring `nice_nano_v2` as the board identifier is the highest-priority change.

Secondary concerns include PMW3610 driver compatibility (the badjeff community driver has undergone multiple API iterations), potential devicetree conflicts from manually-defined input event codes, and minor Kconfig renames in the Bluetooth controller power settings. The west.yml manifest structure itself appears valid.

A critical limitation of this research is that WebSearch, WebFetch, and GitHub API access were all unavailable, so all findings are based on training data and local codebase analysis. Every recommendation should be validated against current GitHub repos before implementation.

## Key Findings

**Stack:** Board name `nice_nano` must likely become `nice_nano_v2`; PMW3610 driver Kconfig/compatible strings need verification against current badjeff main branch.
**Architecture:** No structural changes needed -- shield definitions, keymap format, and west.yml manifest approach are all forward-compatible.
**Critical pitfall:** Building against the wrong board definition produces firmware that boots into an incorrect hardware configuration, causing "device does nothing" symptoms with no obvious error.

## Implications for Roadmap

Based on research, suggested phase structure:

1. **Board Name Fix and Minimal Build** - Fix the board name in build.yaml, attempt a build, and check build logs for errors
   - Addresses: Primary "device does nothing" symptom
   - Avoids: Making multiple changes simultaneously, obscuring the root cause

2. **PMW3610 Driver Verification** - Verify driver compatibility, update compatible strings and Kconfig if needed
   - Addresses: Trackball functionality
   - Avoids: Assuming the community driver works without verification

3. **Devicetree Cleanup** - Remove manual input event code defines, remove deprecated label properties
   - Addresses: Build warnings, potential redefinition conflicts
   - Avoids: Silent build issues that may cause runtime problems

4. **Kconfig Audit** - Verify all Kconfig options still exist, fix any renames
   - Addresses: BLE TX power, experimental connection options
   - Avoids: Degraded BLE performance from missing config

5. **Pin Revisions** - After successful build and hardware test, pin west.yml to known-good commits
   - Addresses: Future breakage from upstream changes
   - Avoids: Premature pinning before migration is validated

**Phase ordering rationale:**
- Board name fix must come first because it is the most likely cause of total device failure
- PMW3610 is second because trackball is a core feature that depends on a community driver
- Devicetree and Kconfig changes are lower risk and can be done incrementally
- Pinning comes last because you need a working build first

**Research flags for phases:**
- Phase 2 (PMW3610): Needs deeper research -- must check badjeff repo for current API
- Phase 4 (Kconfig): Standard patterns but needs build-log-driven verification

## Confidence Assessment

| Area | Confidence | Notes |
|------|------------|-------|
| Board name change | MEDIUM | Strong signal from codebase history but exact name needs GitHub verification |
| West.yml structure | HIGH | Format confirmed from local inspection, unchanged between Zephyr versions |
| PMW3610 driver | LOW | Community driver evolves independently; cannot verify without repo access |
| Kconfig renames | LOW-MEDIUM | Training data suggests some changes but specifics unverifiable |
| Devicetree changes | MEDIUM | Label deprecation well-documented; input code conflicts identifiable from local code |

## Gaps to Address

- Exact board name in current ZMK main (need to check zmkfirmware/zmk app/boards/ directory)
- Current PMW3610 driver API (compatible string, Kconfig prefix, new required properties)
- Whether `CONFIG_BT_CTLR_TX_PWR_PLUS_8` still exists in Zephyr 4.x
- Whether `CONFIG_ZMK_BLE_EXPERIMENTAL_CONN` was graduated or renamed
- Exact Zephyr version that ZMK main currently targets (expected 4.0 or 4.1)

---

*Research summary: 2026-03-18*
