# Codebase Concerns

**Analysis Date:** 2026-03-18

## Known Issues

**Trackball Auto-Mouse Layer Switching Bug:**
- Issue: Moving the trackball unintentionally switches the Fun layer instead of maintaining Auto-Mouse behavior
- Files: `config/charybdis.keymap`
- Trigger: Moving the trackball while on the Base layer causes navigation to Fun layer
- Cause: After inserting a new layer (SwapCtrlGui) as layer 1, something is hardcoded to point to a specific layer ID rather than by layer name
- Current workaround: Mouse clicks were added to the Fun layer in commit fe306d5 to provide mouse functionality there as well
- Investigation status: Acknowledged but incomplete - notes indicate "Maybe something is hardcoded to point to a specific layer rather than layername?"
- Fix approach: Review ZMK input processors and trackball listener configuration in `config/charybdis.keymap` (lines 13-26) to use layer names instead of layer indices, or refactor layer ordering to prevent hardcoded references

**Deprecated PMW3610 Configuration Settings:**
- Issue: Orientation settings in config appear obsolete
- Files: `config/boards/shields/charybdis/charybdis_right.conf`
- Status: Lines 10-11 marked with "Note: These appear to be obsolete as of Oct 2025, replaced by flags in pmw3610.dtsi"
- Commented out settings:
  - `CONFIG_PMW3610_ALT_INVERT_X=y`
  - `CONFIG_PMW3610_ALT_INVERT_Y=y`
- Replaced by: Hardware flags in `config/boards/shields/charybdis/pmw3610.dtsi` (lines 51-54)
- Fix approach: Remove the obsolete commented lines from charybdis_right.conf to reduce configuration confusion

## Fragile Areas

**Trackball Sensitivity Configuration:**
- Files: `config/charybdis.keymap`, `config/boards/shields/charybdis/pmw3610.dtsi`
- Why fragile: Multiple configuration touchpoints affect mouse behavior:
  - CPI (sensitivity) setting in `pmw3610.dtsi` line 49: `cpi = <800>;`
  - Input processor scaling in `charybdis.keymap` lines 16 and 23 (1x2 for normal, 1x20 for scroll)
  - XY swap and axis inversion flags in `pmw3610.dtsi` lines 52-54
  - Power settings in `charybdis_right.conf` lines 14, 15, 23, 26
- Safe modification: Changes to sensitivity require testing on actual hardware; scroll layer scaling affects only layer 5
- Test coverage: No visible unit tests; relies on hardware validation only

**Layer System Architecture:**
- Files: `config/charybdis.keymap` (keymap layers 0-5 defined lines 103-159)
- Why fragile: Evidence suggests hardcoded layer ID references exist somewhere in the ZMK framework or configuration
  - Layer switching by index instead of name caused the auto-mouse bug (see Known Issues)
  - Inserting SwapCtrlGui as layer 1 broke auto-mouse functionality
- Safe modification: Layer reordering should be avoided; new layers should be appended only
- Documentation: Layer purposes not documented in keymap; layer indices are:
  - 0: BASE
  - 1: SwapCtrlGui (Mac alternative)
  - 2: NUM (numbers and symbols)
  - 3: NAV (navigation)
  - 4: FUN (function keys and Bluetooth)
  - 5: SCROLL (mouse scrolling)

**Hardware GPIO and SPI Configuration:**
- Files: `config/boards/shields/charybdis/charybdis.dtsi`, `config/boards/shields/charybdis/pmw3610.dtsi`
- Why fragile: Hardware pinout is tightly coupled to specific NRF pins
  - Trackball SPI: pins 8, 17, 17, 20, 6 (lines 17-46 in pmw3610.dtsi)
  - Matrix rows: pins 18, 5, 4, 9 (lines 41-46 in charybdis.dtsi)
  - Matrix columns: pins 19, 20, 10, 6, 7, 8 (lines 12-19 in charybdis_left.overlay)
- Risk: Pin conflicts or changes break hardware functionality completely
- Safe modification: Pin changes require hardware validation; coordinate with actual PCB layout

## Scaling Limits

**Bluetooth Connection Count:**
- Current capacity: 5 Bluetooth device slots (BT_SEL 0-4 in keymap line 144-145)
- Configured in: `config/charybdis.conf` (implicit via ZMK defaults)
- Scaling path: If more devices needed, would require ZMK firmware configuration changes (not in user config)

**Keymap Complexity:**
- Current: 6 layers (BASE, SwapCtrlGui, NUM, NAV, FUN, SCROLL)
- Combos: 8 combos defined (lines 29-71)
- Custom behaviors: 3 custom hold-tap behaviors (lines 73-100)
- Scaling risk: ZMK has undocumented limits; layer insertion already caused issues with hardcoded layer references

## Dependencies at Risk

**ZMK Framework Version:**
- Current: ZMK main branch (unpinned as of commit 43c1554)
- Risk: Unpinning zephyr allows breaking changes from upstream ZMK
- Impact: PMW3610 driver API changes have already occurred (CONFIG_PMW3610_* deprecated to CONFIG_PMW3610_ALT_*)
- Migration history:
  - Pinned to v0.2.1 in f496ed7, then to v0.3 in fd4d604
  - Currently on main branch (commit 43c1554 unpinned it)
- Recommendation: Pin to stable release versions; verify configuration compatibility after ZMK updates

**External Driver Dependency:**
- Package: `zmk-pmw3610-driver`
- Remote: `https://github.com/badjeff/zmk-pmw3610-driver`
- Configured in: `config/west.yml` lines 12-14
- Risk: Driver is community-maintained, not part of official ZMK; API changes (orientation settings) indicate active development
- Mitigation: Pin driver revision in west.yml to known-working commit; test after updates

**ZMK Version Tag in Build:**
- Issue: build.yaml removed nice_nano version tag in commit 7601b0e
- Files: `build.yaml` line 16-19
- Risk: Using unversioned board reference may pull incompatible board definitions
- Recommendation: Restore explicit nice_nano@2.0.0 version pinning

## Configuration Obsolescence

**Commented-Out Power Settings:**
- Files: `config/boards/shields/charybdis/charybdis_right.conf` lines 16-20
- Content: Heavily commented REST1/REST2/REST3 power modes
- Impact: Unclear if these are truly unsupported or just tuning options
- Recommendation: Document which settings are actually supported in current badjeff driver version, remove unusable options

**Extra Power-Up Delay:**
- Files: `config/boards/shields/charybdis/charybdis_right.conf` line 26
- Setting: `CONFIG_PMW3610_ALT_INIT_POWER_UP_EXTRA_DELAY_MS=1000`
- Issue: Described as "If you have power-up issues" but unconditionally enabled
- Recommendation: Document whether this is required for this specific hardware or a workaround for occasional issues

## Missing Documentation

**Layer Purpose Documentation:**
- Problem: Keymap contains 6 layers with no comments explaining their purpose or intended use
- Files: `config/charybdis.keymap` lines 106-159
- Impact: Makes it difficult to understand if changes maintain intended functionality
- Recommendation: Add JSDoc-style comments above each layer definition

**Combo Positioning Logic:**
- Problem: Combo key-positions are numeric indices with no visual mapping
- Files: `config/charybdis.keymap` lines 32-70
- Example: `key-positions = <27 28>;` (LBracket combo) requires counting to verify correctness
- Recommendation: Add comments mapping indices to physical keys or use ZMK's named key positions if available

**Trackball Scaling Rationale:**
- Problem: No explanation for why scroll uses 1:20 scaling while navigation uses 1:2
- Files: `config/charybdis.keymap` lines 16, 23
- Recommendation: Document scaling choice and provide instructions for sensitivity tuning

## Test Coverage Gaps

**No Hardware Validation Coverage:**
- What's not tested: Trackball operation, Bluetooth pairing, layer switching behavior
- Files: All firmware configuration files - no test suite exists
- Risk: Unpinned ZMK updates could silently break functionality without detection
- Priority: High - firmware bugs manifest only on physical hardware

**No Integration Testing:**
- Problem: No automated verification of combo positions, layer key bindings, or behavior interactions
- Risk: Manual testing only; easy to introduce key mapping errors
- Recommendation: Create hardware test checklist or consider ZMK testing framework if available

## Build and Deployment Concerns

**Build Matrix Consistency:**
- Files: `build.yaml` lines 15-23
- Issue: Builds both charybdis_left and charybdis_right, but also a settings_reset board
- Risk: Three separate builds increases chance of configuration drift between halves
- Recommendation: Ensure settings_reset board matches charybdis board/shield configuration

---

*Concerns audit: 2026-03-18*
