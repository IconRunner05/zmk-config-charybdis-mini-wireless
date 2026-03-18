# Architecture

**Analysis Date:** 2026-03-18

## Pattern Overview

**Overall:** Device Configuration and Firmware Build System

This is a ZMK firmware configuration repository for the Charybdis Mini split keyboard. The architecture follows a declarative, hardware-description-language (HDL) approach where keyboard behavior, key mappings, and hardware connections are defined through configuration files rather than imperative code.

**Key Characteristics:**
- **Split Keyboard Architecture**: Separates left and right half configurations with BLE synchronization
- **Multi-Layer Keymaps**: Support for 6 independent keyboard layers (BASE, SwapCtrlGui, NUM, NAV, FUN, SCROLL)
- **Input Processing Pipeline**: Trackball sensor data flows through transformation processors before output
- **Declarative Configuration**: Hardware and behavior defined via device tree (`.dtsi`), configuration (`.conf`), and keymap (`.keymap`) files
- **Build-Time Assembly**: West manifest orchestrates dependencies; GitHub Actions builds firmware artifacts

## Layers

**Hardware Abstraction Layer (HAL):**
- Purpose: Defines physical keyboard matrix, pinouts, and sensor connections
- Location: `config/boards/shields/charybdis/*.dtsi`, `config/boards/shields/charybdis/*.overlay`
- Contains: GPIO mappings, SPI configuration, keyboard matrix transforms, trackball sensor setup
- Depends on: ZMK framework (imported via `west.yml`), NRF chip abstractions
- Used by: Input processing layer (trackball listener, key scanner)

**Device Configuration Layer:**
- Purpose: Declares what features are enabled and their parameters
- Location: `config/boards/shields/charybdis/*.conf`, `config/charybdis.conf`
- Contains: Configuration flags (e.g., `CONFIG_ZMK_POINTING=y`, `CONFIG_PMW3610_ALT=y`), parameters (e.g., `CONFIG_PMW3610_ALT_CPI=800`)
- Depends on: Kconfig build system
- Used by: Build system to enable/configure features at compile time

**Behavior & Input Processing Layer:**
- Purpose: Transforms raw hardware events into keyboard/pointer output
- Location: `config/charybdis.keymap`, `config/charybdis.conf`
- Contains: Input processors (trackball sensitivity scaling, scroll mapping), key combos, custom behaviors, layer definitions
- Depends on: Hardware layer (trackball device reference), ZMK behavior definitions
- Used by: Output (USB/BLE events sent to host)

**Keymap Definition Layer:**
- Purpose: Maps physical keys and behaviors to output events
- Location: `config/charybdis.keymap`
- Contains: 6 keymap layers (BASE, SwapCtrlGui, NUM, NAV, FUN, SCROLL), combo definitions, custom hold-tap behaviors
- Depends on: ZMK key bindings (`dt-bindings/zmk/keys.h`), mouse bindings (`dt-bindings/zmk/mouse.h`), behaviors
- Used by: Firmware runtime to process key press events

**Build & Manifest Layer:**
- Purpose: Orchestrates dependencies and build configuration
- Location: `config/west.yml`, `build.yaml`
- Contains: ZMK firmware source import, PMW3610 driver module, build matrix (board/shield combinations)
- Depends on: GitHub repositories (zmkfirmware/zmk, badjeff/zmk-pmw3610-driver)
- Used by: GitHub Actions CI/CD pipeline

## Data Flow

**Key Press Event Processing:**

1. Physical key press → GPIO matrix scanner (`kscan0` in `charybdis.dtsi`)
2. Matrix scanner detects row/column intersection via `row-gpios` and `col-gpios`
3. Key position mapped through transformation matrix (`default_transform` or `five_column_transform`)
4. Mapped key position looked up in active keymap layer (`charybdis.keymap`)
5. Binding executed (key press, layer toggle, combo trigger, behavior invocation)
6. Output event sent via USB/BLE to host

**Trackball Input Processing:**

1. PMW3610 sensor detects relative motion via SPI (`spi0` in `pmw3610.dtsi`)
2. Input listener (`trackball_listener` in `charybdis.keymap`) receives motion events
3. Input processors apply transformations:
   - `zip_xy_scaler 1 2`: Scale Y axis by 2x (sensitivity adjustment)
   - Layer-specific override: When in SCROLL layer, Y-axis scaled by 20x and mapped to scroll wheel
   - Optional: `zip_input_transform` applies inversions/axis swaps
4. Processed coordinates output as mouse movement or scroll events
5. Events sent via USB/BLE to host

**Configuration Application:**

1. Build system reads `build.yaml` (board/shield matrix)
2. West manifest (`west.yml`) clones ZMK firmware and PMW3610 driver
3. Build process loads `.conf` files in order (global, then board-specific, then shield-specific)
4. Device tree includes assemble full hardware model
5. Kconfig system merges configuration, respects conditional enables
6. Firmware compiled with merged configuration

**State Management:**
- **Ephemeral State**: Current keymap layer (volatile, modified by key presses)
- **Persistent State**: Bluetooth pairing bonds, output selection preference (stored in NRF flash)
- **Battery State**: Monitored via `vbatt` device, reported to central device

## Key Abstractions

**Keymap Layer:**
- Purpose: Group of key bindings representing alternate input mode (numpad, navigation, media control)
- Examples: `BASE`, `NUM`, `NAV`, `FUN`, `SCROLL` in `config/charybdis.keymap` (lines 106-157)
- Pattern: Each layer is a 4-row, 12-column grid with some keys marked transparent (`&trans`) to fall through to layer below

**Input Processor Chain:**
- Purpose: Pipeline of transformations applied to raw trackball input
- Example: `<&zip_xy_scaler 1 2>` applies X scale of 1, Y scale of 2
- Pattern: Processors are registered in `trackball_listener` device, applied in order

**Combo (Key Combination):**
- Purpose: Multiple simultaneous key presses trigger special binding
- Examples: Delete (K+L keys), Backspace (K+J keys), Emoji (Y+X keys)
- Pattern: Defined in `combos` node, uses `key-positions` to reference physical key indices
- Located in: `config/charybdis.keymap` (lines 29-71)

**Behavior (Custom Key Binding):**
- Purpose: Key press with additional logic (hold vs. tap, key repetition, conditionals)
- Examples: `u_mt` (mod-tap), `u_lt` (layer-tap), `Shift_Enter` (shift-hold-enter-tap)
- Pattern: `compatible = "zmk,behavior-hold-tap"`, defines tap and hold bindings separately
- Located in: `config/charybdis.keymap` (lines 73-101)

**Shield Configuration:**
- Purpose: Defines left/right half specifics without duplicating common hardware
- Examples: `charybdis_left.overlay` (trackball disabled) vs. `charybdis_right.overlay` (trackball enabled)
- Pattern: Includes base `charybdis.dtsi`, then `&kscan0 { col-gpios = ... }` override and device status changes
- Located in: `config/boards/shields/charybdis/charybdis_left.overlay`, `charybdis_right.overlay`

## Entry Points

**Firmware Build Entry Point:**
- Location: `build.yaml`
- Triggers: GitHub Actions push/pull request, workflow dispatch
- Responsibilities: Defines build matrix (nice_nano board × charybdis_left/right shields) and compilation flags

**Keymap Entry Point:**
- Location: `config/charybdis.keymap`
- Triggers: Firmware startup (keymap loaded into memory), persistent across runtime
- Responsibilities: Define all key behaviors, layers, combos, input processors

**Hardware Entry Point:**
- Location: `config/boards/shields/charybdis/charybdis.dtsi` (shared), `charybdis_left.overlay` / `charybdis_right.overlay` (specific)
- Triggers: Bootloader → device tree compilation
- Responsibilities: Declare GPIO matrices, SPI sensors, physical layout, transforms

**Device Configuration Entry Point:**
- Location: `config/boards/shields/charybdis/charybdis.conf`, `charybdis_right.conf`
- Triggers: Build system Kconfig parsing
- Responsibilities: Enable/disable features, tune sensor parameters, set keyboard name and split role

## Error Handling

**Strategy:** Prevention through compile-time validation and device tree constraints

**Patterns:**
- **Conditional Build Flags**: `if SHIELD_CHARYBDIS_RIGHT` guards BLE central role to prevent dual-central misconfiguration
- **Key Position Bounds**: Matrix size (4 rows × 12 columns = 48 keys) enforced by transform definitions; combos use indices within bounds
- **GPIO Conflict Avoidance**: Different pin assignments per shield (left vs. right) prevent pin reuse
- **Configuration Validation**: ZMK build system validates Kconfig options; invalid combinations fail at build time
- **No Runtime Recovery**: If keyboard enters bad state, user must reflash firmware or reset via bootloader

## Cross-Cutting Concerns

**Logging:** Not applicable - embedded firmware with minimal debug output

**Validation:** Compile-time through:
- Device tree syntax validation
- Kconfig option constraint checking
- Matrix transform bounds checking
- Key position array length matching keymap dimensions

**Hardware Discovery:** Automatic via device tree:
- Each shield declares features (`features: [keys, pointer]` in `charybdis.zmk.yml`)
- ZMK framework auto-configures necessary drivers
- Left/right identity determined by build matrix target (shield name)

**Bluetooth Connectivity:** Managed by ZMK layer:
- Central role (right half): Initiates connections, relays input from peripheral to host
- Peripheral role (left half): Connects to central, sends key/trackball events to central
- Bonding state: Persistent via NRF flash storage, survives reboot
- Output toggle: `&out OUT_TOG` key switches USB ↔ BLE

---

*Architecture analysis: 2026-03-18*
