# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

ZMK firmware config for a Charybdis split keyboard (Bastardkb design, `pro_micro` form factor on `nice_nano_v2`) with a PMW3610 trackball on the right half. Right half is the BLE central; both halves run the same shield with side-specific overlays. Builds locally via Docker against the same image as GitHub Actions CI (`zmkfirmware/zmk-build-arm:stable`, pinned to ZMK v0.2.1 / Zephyr v3.5.0+zmk-fixes) — no host Zephyr SDK required.

## Branches

| Branch | Hardware | Matrix | Thumbs | Notes |
|---|---|---|---|---|
| `master` | Charybdis **Mini** Wireless (3×5 + outer pinky col) | 12 cols × 4 rows | 3L + 2R = 5 | PCB-wired |
| `charybdis-full` | Charybdis **Full** (4×6) | 12 cols × 5 rows | 5L + 3R = 8 (outer row long) | Hand-wired. 5th `row-gpios` pin on `pro_micro 15` is the new number-row. Same trackball, same PMW3610 config, same Makefile, same shield names — only keymap/transform/layout differ. |

Shield names (`charybdis_left`, `charybdis_right`), `build.yaml` matrix, Makefile targets, and resulting `.uf2` filenames are identical between branches. The Docker volume `zmk-workspace` can be reused across branch checkouts because the west manifest is unchanged.

**Key-position indices (Full branch) — for combos / hold-trigger-key-positions:**
- Number row: 0-11 | TAB row: 12-23 | ESC row: 24-35 | LGUI/Z row: 36-47 | Thumb row: 48-55
- Thumb cluster: 48-50 = L outer (T1-T3), 51-52 = L inner (T4-T5), 53-54 = R outer (T6-T7), 55 = R inner (T8)

## Build Commands

All builds run inside Docker. The Makefile is the entry point — never invoke `west` directly from the host.

| Command | Purpose |
|---|---|
| `make init` | First-time setup: create volume, pull image, `west init/update/zephyr-export` (~1–2 GB download) |
| `make build` | Build all three targets (left, right, reset) |
| `make left` | Build `charybdis_left` only |
| `make right` | Build `charybdis_right` with `studio-rpc-usb-uart` snippet + `CONFIG_ZMK_STUDIO=y` |
| `make reset` | Build `settings_reset` flasher |
| `make update` | Re-run `west update` after editing `config/west.yml` |
| `make pristine` | Wipe build caches inside the Docker volume (modules stay) |
| `make clean` | Full teardown: remove volume, sentinel, staging dirs |
| `make firmware` | List `.uf2` artifacts in `firmware/` |

CI workflow `.github/workflows/build.yml` is a one-liner delegating to `zmkfirmware/zmk/.github/workflows/build-user-config.yml@v0.2.1` — pinned to the same tag as `west.yml`. CI matrix comes from `build.yaml` (3 targets). Local `make build` must stay equivalent to that matrix.

## Critical Build Architecture

**Why a named Docker volume instead of a bind-mount?** Docker Desktop on macOS uses VirtioFS for host bind-mounts. Git's pack-file inflate during `west update` silently corrupts on VirtioFS ("unknown compression method"). The named volume `zmk-workspace` lives entirely inside Docker's Linux VM, so all git I/O is native.

**Why staging dirs at `~/Docker/zmk-config` and `~/Docker/zmk-fw`?** The repo path lives inside `~/Library/Mobile Documents/...` (iCloud, with spaces). Docker `-v` cannot reliably bind spaced paths and iCloud sync interferes with build I/O. The Makefile `rsync`s `config/` into a space-free staging dir before each build and extracts `.uf2` artifacts via an Alpine helper container.

**Sentinel file** `~/Docker/.zmk-initialized` gates all build targets — `make init` touches it after a successful `west init/update/zephyr-export`; every other target's `check_init` macro fails fast if it's missing. Deleting the sentinel without also resetting the volume will cause `west init` to re-run against an existing workspace (harmless but noisy).

**Three layers exist for outputs:**
1. `/workspace/build/<target>/zephyr/zmk.uf2` — inside Docker volume
2. `~/Docker/zmk-fw/` — host staging (space-free, Docker-writable)
3. `firmware/` in repo root — final copy (gitignored alongside `build/`, `.zmk-workspace/`, `.west/`)

Editing `Paths` in `Makefile` (lines ~36–57) is the supported way to relocate any of these. Do not move config out of `config/` — the rsync source is hard-coded.

## Source Layout

- `config/west.yml` — west manifest. Pins:
  - `zmk@v0.2.1` (zmkfirmware)
  - `badjeff/zmk-pmw3610-driver@main` — the `pixart,pmw3610-alt` driver
  - `IconRunner05/zmk-input-processor-report-rate-limit@main` — user's own fork providing the BLE-only rate limiter
- `config/charybdis.conf` — top-level Kconfig overlays (BLE tuning, pointing, behaviors queue 512).
- `config/charybdis.keymap` — keymap, combos, behaviors, trackball input-processor chain.
- `config/info.json` — keymap-editor layout descriptor.
- `config/boards/shields/charybdis/`
  - `charybdis.dtsi` — matrix-transform (12×4 with row-2-col diode direction), `vbatt`, `kscan0` row GPIOs (pro_micro 18/5/4/9). Left/right define their own `col-gpios`.
  - `charybdis-layouts.dtsi` — physical layout for ZMK Studio.
  - `pmw3610.dtsi` — SPI0 + `trackball` node. CPI, `swap-xy`/`invert-x`/`invert-y` live here.
  - `charybdis_left.overlay` — col-gpios + **disables `&spi0` and `&trackball`** (no sensor on left).
  - `charybdis_right.overlay` — col-gpios + `col-offset = 6` (default) / `5` (5-col) for the transforms.
  - `charybdis_left.conf` — empty.
  - `charybdis_right.conf` — PMW3610 power/perf tuning (`RUN_DOWNSHIFT_TIME_MS=3264`, `REST1_SAMPLE_TIME_MS=40`, `SMART_ALGORITHM=y`, `INIT_POWER_UP_EXTRA_DELAY_MS=1000`). Old `INVERT_X/Y` Kconfigs are commented out as obsolete.
  - `Kconfig.defconfig` — right is BLE central; both halves get `ZMK_SPLIT=y`. `ZMK_KEYBOARD_NAME="Charybdis"` set on right only.
  - `charybdis.zmk.yml` — shield metadata (`studio: true`, features `keys` + `pointer`).
  - `settings_reset` shield comes from upstream ZMK, not this repo.

## Trackball / Input-Processor Chain

The trackball pipeline is the most fragile part of this config. It lives in `config/charybdis.keymap` under `trackball_listener` and `config/boards/shields/charybdis/pmw3610.dtsi`.

1. **Noise gate first.** `zip_ble_report_rate_limit <ms>` must be the **first** processor in the chain so it operates on raw sensor values, not scaled ones. It's the BLE-only variant from `IconRunner05/zmk-input-processor-report-rate-limit` — USB output is intentionally not rate-limited (commit `c9d27e0`). Current value is `8` ms. The doc comment block at the top of `charybdis.keymap` documents the tradeoff table (8/12/16/20/33 ms) — keep it accurate when changing the value.
2. **Sensitivity has two knobs.** CPI in `pmw3610.dtsi` (`cpi = <900>`) is hardware-side; `zip_xy_scaler 1 N` in the keymap is the software divisor (currently `1 2` on base). The SCROLL layer (layer 5) re-scales aggressively (`1 20`) then maps XY→scroll with Y inverted.
3. **Sensor orientation** (`swap-xy`, `invert-x`, `invert-y`) is set in `pmw3610.dtsi`. The Kconfig equivalents (`CONFIG_PMW3610_ALT_INVERT_X/Y`) in `charybdis_right.conf` are commented out as obsolete (Oct 2025) — do not re-enable; the dtsi flags are authoritative.
4. **History context.** Recent commits removed ZMK sleep and dropped `PMW3610_ALT_REPORT_INTERVAL_MIN` in favor of the input-processor approach. Don't reintroduce the old Kconfig knob without checking the rate limiter is still active.

## Keymap

Six layers. Layer numbers are referenced directly in combos, mod-taps, and the scroll processor — do not renumber without sweeping all callers.

| # | Name | Purpose |
|---|---|---|
| 0 | BASE | QWERTY, thumb cluster `LCtrl / mo 2 NUM / LShift   Space / lt 3 NAV-BACKSPACE` |
| 1 | SwapCtrlGui | Swap LCtrl ↔ LGUI on thumbs (Win mode). Toggled by combo `24 36` |
| 2 | NUM | Numpad + symbols, `mo 5` access, BT_SEL 0-2 |
| 3 | NAV | Arrows, F-keys, `bt BT_CLR`, `studio_unlock`, `bootloader`, `out OUT_TOG`, `mo 4` on right thumb |
| 4 | FUN | F1-F12, BT_SEL 0-4 + BT_CLR/BT_CLR_ALL, media, `to 0` |
| 5 | SCROLL | All `&trans` — exists so `trackball_listener`'s `scroll` child can match `layers = <5>` |

Custom behaviors `u_mt` / `u_lt` (200ms tap-preferred mod-tap / layer-tap) and `Shift_Enter` (100ms, hold-trigger-key-positions=40). Combos cover Delete/Backspace/Enter/brackets, emoji picker (`LG(SEMICOLON)`), and the Mac↔Win layer toggle.

## Trackball Debug Scripts

`scripts/trackball_debug.sh` (macOS) and `scripts/trackball_debug.ps1` (Windows) are independent debuggers — they don't share code or transport.

- **macOS**: streams `log stream --process bluetoothd --level debug`, filters by hard-coded BLE UUID `16B237D6-4DB2-6D6C-5F03-223F2057868D`. Update that UUID if pairing changes. Needs `sudo` (self-elevates). Flags: `--errors-only` / `-e` / `-q` / `--quiet`. Parses RSSI, TX/RX `S=/F=` counters, topology renegotiation, congestion/throttle/dropped events.
- **Windows**: PowerShell + WinForms RawInput receiver, **no admin needed**. Filters HID by VID `1D50` / PID `615E` (nice!nano default). Flags: `-Quiet` / `-q` / `-e`, `-VerboseInput` / `-v`, `-StutterThresholdMs <int>` (default 50). Reports stutters on the active device.

Note: `scripts/trackball_debug.ps1` is currently untracked in git — `git status` shows it as `??`.

## Conventions

- Do not commit anything in `firmware/`, `build/`, `.zmk-workspace/`, `.west/` (all gitignored).
- Keep `build.yaml`, the `make` targets, and `west.yml` revision pins in lockstep. CI uses the `@v0.2.1` tag of the reusable workflow — if you bump ZMK, bump both.
- The persistent west workspace inside the `zmk-workspace` Docker volume is the source of truth between builds. `make pristine` wipes only `build/` inside the volume; `make clean` wipes the entire volume and forces a re-`init`.
- Layer numbers and the `layers = <5>` reference in `trackball_listener.scroll` are coupled — change them together.
