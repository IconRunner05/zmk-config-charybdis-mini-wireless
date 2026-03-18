# Coding Conventions

**Analysis Date:** 2026-03-18

## Naming Patterns

**Files:**
- Kebab-case for device tree source files: `charybdis_left.overlay`, `charybdis_right.conf`
- Snake_case for Kconfig files: `Kconfig.shield`, `Kconfig.defconfig`
- Descriptive names following purpose: `charybdis-layouts.dtsi`, `pmw3610.dtsi`
- Vertical variant overlays use underscores: `charybdis_left.overlay`, `charybdis_right.overlay`

**Behaviors:**
- PascalCase with underscore prefix for custom ZMK behaviors: `u_mt`, `u_lt`, `Shift_Enter`
- Label names use PascalCase: `Delete`, `Backspace`, `Enter`, `ToggleMacWin`
- Built-in ZMK behaviors use lowercase: `mo` (momentary), `lt` (layer-tap), `kp` (key press)

**Keymaps and Layers:**
- Layer names in UPPERCASE: `BASE`, `SwapCtrlGui`, `NUM`, `NAV`, `FUN`, `SCROLL`
- Key bindings follow ZMK macro format: `&kp`, `&mo`, `&lt`, `&tog`, `&mkp`, `&bootloader`, `&out`, `&studio_unlock`, `&bt`
- Configuration options use SCREAMING_SNAKE_CASE: `CONFIG_ZMK_POINTING`, `CONFIG_PMW3610_ALT`, `CONFIG_BT_CTLR_TX_PWR_PLUS_8`

**Variables:**
- Preprocessor defines use SCREAMING_SNAKE_CASE: `U_TAPPING_TERM`, `INPUT_EV_KEY`, `INPUT_EL_REL`, `INPUT_REL_X`
- Device references use lowercase with underscores: `trackball`, `kscan0`, `vbatt`, `default_transform`
- Node references in overlays use lowercase: `&kscan0`, `&spi0`, `&trackball`, `&pinctrl`

**Types (DTS/Kconfig):**
- Node identifiers use lowercase with underscores: `kscan`, `trackball`, `vbatt`, `physical_layout0`
- Property names use hyphens: `compatible`, `diode-direction`, `row-gpios`, `col-gpios`, `tapping-term-ms`, `hold-trigger-key-positions`, `cpi`, `spi-max-frequency`
- Boolean properties without values: `status = "okay"`, `status = "disabled"`

## Code Style

**Formatting:**
- No standardized formatter (linter not detected)
- Consistent indentation: tabs in DTS files, spaces in keymap arrays
- Aligned key position arrays for readability in keymaps

**Linting:**
- No linting tools detected (no eslint, prettier, or biome configs)
- Code style follows ZMK community conventions

## Import Organization

**DTS Include Order:**
```
1. Relative includes first: #include "charybdis-layouts.dtsi"
2. Then dt-bindings from framework: #include <dt-bindings/...>
3. System includes last: #include <physical_layouts.dtsi>
```

**DTS Includes Pattern:**
```c
#include "charybdis.dtsi"
#include "pmw3610.dtsi"
#include <behaviors.dtsi>
#include <dt-bindings/zmk/...>
```

**Path Conventions:**
- Relative paths for same-directory includes: `#include "charybdis.dtsi"`
- Angle brackets for framework/system includes: `#include <dt-bindings/zmk/mouse.h>`

## Error Handling

**Patterns:**
- Configuration flags control features: `CONFIG_ZMK_SPLIT`, `CONFIG_ZMK_POINTING`, `CONFIG_ZMK_EXT_POWER`
- Feature detection via Kconfig: conditional sections in `Kconfig.defconfig` check shield type
- Optional settings commented with rationale: `# Optional orientation settings (Note: These appear to be obsolete...)`
- Commented-out alternatives for experimentation: Lines prefixed with `#` in `.conf` files
- Status properties for disabling: `status = "disabled"` in overlay files

## Logging

**Framework:** None - ZMK is firmware, logging limited to compile-time config

**Patterns:**
- Configuration comments for tuning: `# Optional power saving and performance tuning`
- Deprecation notes: `# Note: These appear to be obsolete as of Oct 2025`
- Rationale for settings: `// Decrease sensitivity` above input processor configs

## Comments

**When to Comment:**
- Complex ZMK-specific configurations warrant inline comments
- Non-obvious key position mappings documented above keymaps
- Power tuning settings and their purpose explicitly noted
- Deprecation warnings for obsolete configuration options

**Format:**
- Single-line: `// Comment` in keymaps and DTS files
- Multi-line: `/* Comment */` in DTS headers
- Inline: `# Comment` in configuration files (.conf)

## Function Design

**Size:** Not applicable - firmware configuration project

**Parameters:** N/A - DTS bindings and Kconfig declarations

**Return Values:** N/A - declarative configuration

## Module Design

**Exports:**
- DTS: Device nodes exported via labels for reference in overlays
- Kconfig: Configuration symbols exported for conditional compilation
- Keymaps: Layer definitions and behaviors published as part of root device tree

**Barrel Files:**
- `charybdis.dtsi` includes and re-exports base definitions via `#include` statements
- `charybdis.zmk.yml` provides metadata about shield capabilities and dependencies

## Layout Organization Pattern

**Keymap Structure:**
```
/ {
    [features] {
        [definitions]
    };

    keymap {
        compatible = "zmk,keymap";

        LAYER_NAME {
            bindings = <...>;
        };
    };
};
```

**Configuration Pattern:**
```
CONFIG_FEATURE=y
CONFIG_FEATURE_SETTING=value
# Optional setting (commented with rationale)
# CONFIG_DEPRECATED_SETTING=n
```

## Device Tree Binding Conventions

**Matrix Transform:**
- Use `map = < RC(row,col) ... >` format for physical to logical key mapping
- Define separate transforms for different layouts: `default_transform`, `five_column_transform`

**GPIO Configuration:**
- Reference pins via `&pro_micro` device
- Use tuple format: `<&pro_micro PIN (FLAGS)>`
- Flags include: `GPIO_ACTIVE_HIGH`, `GPIO_ACTIVE_LOW`, `GPIO_PULL_DOWN`, `GPIO_PULL_UP`

**SPI Devices:**
- Define pinctrl groups: `spi0_default`, `spi0_sleep`
- Set compatible property to hardware: `"nordic,nrf-spim"`
- Configure max frequency in Hz: `spi-max-frequency = <2000000>`

---

*Convention analysis: 2026-03-18*
