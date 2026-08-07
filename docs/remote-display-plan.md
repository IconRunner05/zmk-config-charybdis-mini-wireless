# Remote status display — plan & decision ledger

**Goal:** a standalone battery-powered display (XIAO nRF52840 + nice!view) showing
per-half battery, active BLE profile, active layer, and modifier state for the
Charybdis split — with the keyboard remaining **fully functional and unaffected**
when the display is absent.

Last updated 2026-08-07.

---

## Ratified decisions

### D1 — Architecture: BLE broadcast + passive observer
The keyboard broadcasts status inside a BLE **advertisement**; the display is a
passive **observer** that never connects, never pairs, and consumes none of the
keyboard's 5 host profile slots.

**Why:** the owner's hard requirement is that forgetting the display changes
nothing about the keyboard. An observer satisfies this by construction — the
keyboard cannot tell whether anyone is listening.

**Rejected:** a display that is a ZMK split *peripheral* (the split protocol is
one-way — peripherals push events to the central; central state is never exported
back down). Rejected: a display occupying a host profile as a fake computer
(gets battery via BAS and caps-lock via HID, never gets layer or profile, and
burns a profile slot).

### D2 — Keyboard line stays on ZMK v0.2.1
Work continues on the `rewind-7267d42` (36-key) and `full-on-7267d42` (56-key)
branches. The owner reports the v0.2.1 rewind behaves better than the Zephyr 4.1
line, and — decisively — that the earlier dev arcs *"moved too fast and changed
too many things to be able to tell for sure what contributed to or caused the
issue."* Approach from here is deliberate: one variable at a time.

**Consequence:** `master` and `charybdis-full` (both ZMK `main` / Zephyr 4.1) are
**not** the forward path. They remain as reference and as the source of the
stranded fixes catalogued in `crash-lag-investigation.md`.

### D3 — The scanner is version-decoupled from the keyboard
The display is a **separate device with its own firmware image**. Its ZMK version
is therefore independent of the keyboard's. The scanner targets ZMK `main`
(Zephyr 4.1 / LVGL 9.3); the keyboard stays on v0.2.1.

The **only** contract between them is the advertisement payload byte layout.

**Consequence:** the scanner cannot live on the same branch as the keyboard —
one config repo has one `config/west.yml` and therefore one ZMK revision. The
scanner branch is forked from `master` (already on ZMK `main`) and is
**permanently non-merging** with the keyboard branches. This divergence is
intentional; do not "fix" it.

### D4 — Display is the nice!view, both orientations
`sharp,ls0xx`, 160x68 physical, 1 bit per pixel, SPI ≤ 1 MHz. Must support
**landscape** (native 160x68) and **portrait** (logical 68x160). These are
distinct layouts, not one layout rotated — the compositions differ.

---

## Open forks (not yet ruled)

| # | Fork | Status |
|---|---|---|
| F1 | Broadcaster: ready-made kit (Option B) vs own ext-adv set (Option C) | **Deferred to Phase 3.** Leaning C — the kit reaches into ZMK's connectable advertising rather than opening a separate set, which couples the display feature to host pairing. Blocked on whether the kit's keyboard side even compiles on ZMK v0.2.1 (see R1). |
| F2 | Orientation abstraction: compile-time vs runtime vs two artifacts | Out for blind ruling. |

## Open risks

| # | Risk |
|---|---|
| ~~R1~~ | **RESOLVED — favourably.** Keyboard-side status advertisement should compile on ZMK v0.2.1. The *only* version guard in the keyboard-side codebase (`include/zmk/prospector_compat.h`) splits on `KERNEL_VERSION_MAJOR >= 4` — i.e. Zephyr 3.x vs 4.x — and **v0.2.1 and v0.3 are both Zephyr 3.5**, so both take the same branch. Every ZMK API called by `status_advertisement.c` was verified present at tag `v0.2.1` (notably `zmk_keymap_layer_name`, whose rename from `layer_label` landed *before* v0.2.1). Only `src/status_advertisement.c` compiles for a keyboard build; the scanner and adapter units are excluded by Kconfig. **Residual risk is behavioural, not compile-time:** nobody has published a v0.2.1 test, and the piggyback/proxy ADV mode-switching may behave differently on a Zephyr 3.5 host. |
| R2 | The leading crash candidate from `crash-lag-investigation.md` (BLE anchor alignment, commit `97b8436`) is **unmerged and unrun**. It sits on `fix/pmw3610-irq-storm` and is absent from every branch currently being flashed. Three branches carry three different `BT_PERIPHERAL_PREF_LATENCY` values (0 / 8 / 16); none is confirmed. |
| R3 | `rewind-*` / `full-on-*` float `zmk-pmw3610-driver: main` **unpinned**, while upstream shipped a breaking rename (`pixart,pmw3610-alt`, `CONFIG_PMW3610_ALT_*`). These builds can break from an upstream push with no local change. Pin to a SHA. |

---

## Phase plan

Incremental: every phase ends with a commit + push. Tags mark revert points.

| Phase | Work | Gate | Parallel? |
|---|---|---|---|
| 0 | Close the stale-bond rename test (`CharyFull-T1`); record verdict in `crash-lag-investigation.md` | Verdict written | — |
| 1 | Fork `display/mini` ← `rewind-7267d42`, `display/full` ← `full-on-7267d42`. Pin the trackball driver SHA (R3). Decide whether to carry `97b8436` (R2) as its own isolated commit. Tag `baseline-v1` | Builds clean in CI | — |
| 2 | **Soak.** Flash the non-debug build, drive a week. No commits | No disconnects / no hang | — |
| 3 | Broadcaster. Tag `pre-broadcast` first. Resolve F1 | Keyboard still pairs + reconnects with the module disabled *and* enabled | — |
| 4 | **Scanner firmware** — this document's active work | Renders live data in both orientations | **Yes — zero keyboard risk, runs now** |

**Revert story for Phase 3:** `git checkout -b display/broadcast-custom pre-broadcast`.
Phases 0–2 survive untouched; only Phase 3 is discarded. Phase 4 is unaffected —
the scanner does not care which broadcaster feeds it, only the payload layout, so
the byte format is kept stable across the B→C pivot and the display never notices.

**Phase 2 discipline note:** judge nothing on the debug build. Per
`crash-lag-investigation.md`, its logging thread costs ~40% CPU and invalidates
any pointer-feel or latency assessment.

---

## Branch map

```
master              ZMK main   — reference only, not the forward path
charybdis-full      ZMK main   — reference only (56-key, modern)
fix/pmw3610-irq-storm  ZMK main — holds the stranded BLE anchor fix (R2)

rewind-7267d42      ZMK v0.2.1 — 36-key keyboard line   ─┐ forward path
full-on-7267d42     ZMK v0.2.1 — 56-key keyboard line   ─┘

display/scanner     ZMK main   — scanner device; NEVER merges to the above (D3)
```

---

## Phase 4 — confirmed hardware facts

Researched against ZMK `main` and `zmkfirmware/zephyr @ v4.1.0+zmk-fixes`.
These are verified from source, not from docs. Do not re-litigate.

**Board:** `xiao_ble/nrf52840/zmk`. ZMK ships a board *extension* at
`app/boards/seeed/xiao_ble/` (`board.yml` → `extend: xiao_ble`), which adds
`ZMK_USB`, `ZMK_BLE`, UF2 output, NVS/settings, retention boot mode and a
battery divider. `seeeduino_xiao_ble` is legacy; plain `xiao_ble` builds but
silently loses every ZMK defconfig.

**SPI:** use `&xiao_spi` (= `spi2`), already enabled, stock pinctrl
SCK **P1.13/D8**, MOSI **P1.15/D10**, MISO **P1.14/D9**. Upgrade compatible to
`nordic,nrf-spim`.
- `spi3` is **unusable** — default pins P0.21/P0.20/P0.24 collide exactly with
  QSPI SCK/IO0/IO1, and ZMK's board dts enables `&qspi` for the onboard flash.
- `spi1` is **unusable** — shares peripheral instance 1 with `i2c1`, enabled by
  default on XIAO.

**Chip select is ACTIVE HIGH.** Zephyr's `ls0xx.c` declares `SPI_CS_ACTIVE_HIGH`;
`spi.h` requires the GPIO flag to agree, so `GPIO_ACTIVE_HIGH` is mandatory, not
stylistic. Every in-tree `nice_view_spi` uses it. The driver also sets
`SPI_HOLD_ON_CS | SPI_LOCK_ON` — **never put a second device on this bus.**

**A keymap and a kscan are mandatory even with zero keys.** ZMK `main` hard-fails
otherwise (`app/src/keymap.c` `#error "Keymap node not found"`;
`physical_layouts.c` leaves `layouts[]` undefined without a physical layout,
chosen matrix-transform, or chosen kscan). Use `zmk,kscan-mock` with
`rows/columns/events = <0>` — legal per the binding, and burns no GPIO.
`CONFIG_ZMK_STUDIO` must be `n` (Studio hard-requires physical-layout nodes).

**BLE observer:** `CONFIG_BT_OBSERVER` and `CONFIG_BT_PERIPHERAL` are independent
Zephyr roles and do not conflict. Keep **`CONFIG_ZMK_BLE=y`** — `bt_enable()` is
called *only* from ZMK's own `app/src/ble.c`, so disabling it means owning stack
init yourself, which no known config does. Add `CONFIG_BT_OBSERVER=y`,
`CONFIG_ZMK_SPLIT=n`, `CONFIG_PM_DEVICE=n`.

**Use PASSIVE scanning** (`BT_LE_SCAN_TYPE_PASSIVE`). The upstream reference uses
*active* scanning, which transmits SCAN_REQ packets — pointless for a
broadcast-only payload and it costs power on a battery device.

**Integration gotcha:** the `nice_view` shield's `Kconfig.defconfig` implies
`NICE_VIEW_WIDGET_STATUS`, whose `custom_status_screen.c` reads *local* keyboard
state and would collide with ours. Set `CONFIG_NICE_VIEW_WIDGET_STATUS=n` so
ZMK's CMake guard excludes its sources, and supply our own
`zmk_display_status_screen()`.

**Shield order matters:** the shield defining `nice_view_spi` must be listed
before `nice_view` → `shield: charybdis_scanner nice_view`. The board-specific
overlay must be named `boards/xiao_ble_nrf52840_zmk.overlay`.

---

## The wire contract — 26-byte status advertisement

**This is the only coupling between keyboard and scanner (D3).** Keeping it
stable is what makes the Phase 3 B→C pivot free: swap the broadcaster, the
display never notices.

Verified against `t-ogura/prospector-zmk-module`,
`include/zmk/status_advertisement.h` + `src/status_advertisement.c`.
Offsets confirmed by compiling the struct, not inferred.

| Off | Size | Field | Notes |
|----|----|----|----|
| 0 | 2 | `manufacturer_id` | `0xFF 0xFF` — the BLE company ID ("reserved for testing") |
| 2 | 2 | `service_uuid` | `0xAB 0xCD` — **not** a real GATT UUID, just a magic word inside the payload |
| 4 | 1 | `version` | `[7:4]`=major, `[3:0]`=minor |
| 5 | 1 | `battery_level` | **positional, not role-based** — see below |
| 6 | 1 | `active_layer` | written unclamped despite the header's "0-15" comment |
| 7 | 1 | `profile_slot` | `[6]`=dev, `[5:3]`=patch, `[2:0]`=profile. **Must mask `& 0x07`** |
| 8 | 1 | `connection_count` | near-useless — hardcoded 1, +1 if USB HID ready |
| 9 | 1 | `status_flags` | CAPS_WORD 0x01, CHARGING 0x02, USB_CONN 0x04, USB_HID 0x08, BLE_CONN 0x10, BLE_BONDED 0x20 |
| 10 | 1 | `device_role` | 0=standalone, 1=central, 2=peripheral |
| 11 | 1 | `device_index` | |
| 12 | 3 | `peripheral_battery[3]` | `[0]`=other half, `[1]`/`[2]`=aux. 0 = N/A |
| 15 | 4 | `layer_name[4]` | **NOT null-terminated** — fixed 4 bytes, zero-padded |
| 19 | 4 | `keyboard_id[4]` | hash of `hwinfo_get_device_id()`, little-endian host order |
| 23 | 1 | `modifier_flags` | LCTL/LSFT/LALT/LGUI/RCTL/RSFT/RALT/RGUI = bits 0..7 |
| 24 | 1 | `wpm_value` | |
| 25 | 1 | `channel` | 0 = broadcast to all |
| | **26** | | |

On air: `BT_DATA(BT_DATA_MANUFACTURER_DATA, …, 26)` → AD element `0x1B 0xFF | FF FF AB CD …`.
With `BT_DATA_FLAGS` that is 30 of the 31-byte legacy budget — which is *why*
the payload is capped at 26 and why fields are bit-packed.

### Decoder traps — all confirmed in the source

1. **`profile_slot` must be masked `& 0x07`.** The upper bits carry patch/dev
   version. Reading the raw byte yields `0x10 + profile` on current firmware.
2. **`layer_name` is NOT null-terminated**, despite the header comment claiming
   it is. The implementation's own comment says *"Receiver must use `%.4s` or
   memcpy+null, never raw `%s`"*. Copy into a 5-byte buffer and terminate.
3. **The two battery fields are positional (left/right), not role-based.** The
   central *swaps* its own reading based on `CONFIG_ZMK_STATUS_ADV_CENTRAL_SIDE`
   (default `"RIGHT"` — which matches our central). Peripheral-role devices do
   not advertise at all.
4. **Modifiers cannot distinguish left from right** — the writer sets both the L
   and R bit for each modifier class.
5. **`CHARGING` (0x02) has no writer.** Treat as always 0.
6. Matching is a **4-byte magic compare only** (`FF FF AB CD`) on manufacturer
   data ≥26 bytes. No UUID filter, no address filter, no RSSI threshold.
7. Keyboard *name* arrives in a **separate packet** (`BT_DATA_NAME_COMPLETE`),
   which is why upstream uses active scanning. If we use passive scanning we
   forgo the name and must identify keyboards by `keyboard_id` instead — an
   acceptable trade for a single-keyboard display, and it saves TX power.

---

## Widget reuse — corrected

An earlier assumption in this project was that `englmaxi/zmk-dongle-display`
renders to a nice!view. **It does not** — it targets a 128x64 SSD1306/SH1106
**I²C OLED**. No Sharp memory LCD anywhere in that repo.

The actual precedents, and what each is good for:

| Repo | Display | LVGL | Use it for |
|---|---|---|---|
| `englmaxi/zmk-dongle-display` | 128x64 OLED | **9** | The LVGL-9 API forms; widget structure |
| `mctechnology17/zmk-dongle-display-view` | **nice!view 160x68** | 8 (stale, 2024-08) | The 160x68 geometry + layout strategy |
| ZMK in-tree `nice_view` | nice!view 160x68 | 9 | The rotated-mount case only |

**Rotation finding:** `zmk-dongle-display-view` composes directly on the full
160x68 with plain `lv_obj_align()` and **zero canvas rotation**. This confirms
ZMK's square-canvas + `lv_draw_sw_rotate` trick exists *only* because the
nice!view is physically mounted rotated on a keyboard. A desk-standing scanner
in landscape needs no rotation at all — which removes ~18 KB of canvas RAM from
the landscape path.

**Reuse shape:** each upstream widget is a POD state struct + a pure render
function + hard-wired `ZMK_DISPLAY_WIDGET_LISTENER`/`ZMK_SUBSCRIPTION` plumbing.
The render half lifts cleanly; the data half gets deleted and replaced by a call
from our scanner tick. Two known snags: `ZMK_SPLIT_BLE_PERIPHERAL_COUNT` does not
exist on a standalone scanner, and `wpm_status` selects `CONFIG_ZMK_WPM` which we
do not want (WPM arrives at offset 24).

**No existing nice!view scanner exists.** All Prospector forks surveyed use the
ST7789 round LCD; the one alternative port (`dhruvinsh/zmk-prospector`) is
ST7735S. This combination would be the first.

---

## Phase 4 working ledger

| Lane | Purpose | Status |
|---|---|---|
| research/build-facts | XIAO board name, keyless build idiom, observer Kconfig, `nice_view_spi`, CS polarity | **landed** → facts above |
| research/payload | Byte-level advertisement struct, scanner parse/filter, keyboard-side ZMK version floor (R1), widget reuse from `zmk-dongle-display` | running |
| design/orientation | Blind re-derivation of the orientation abstraction (F2) | running |
| author/scaffold | Shield plumbing slice — builds clean + placeholder screen. No BLE, no parsing | running |
