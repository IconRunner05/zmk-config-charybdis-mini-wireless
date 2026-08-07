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

### D6 — Display sleeps on keyboard inactivity
The display must go dark N seconds/minutes after the last keyboard activity and
wake on the next. Rationale: panel longevity and scanner battery life.

**The hard part:** the scanner has no keys, so "last keypress" is not locally
observable. Activity must be *inferred* from the broadcast. Candidate signals,
best first:
1. **Advertising cadence.** The keyboard already switches between an active
   interval (~1 s) and an idle interval (~30 s). A packet gap crossing the idle
   threshold is a strong, zero-cost activity signal that needs no payload change.
2. **Payload delta.** Any change in decoded status (layer, mods, profile,
   battery) implies activity.
3. **A dedicated flag.** `status_flags` has two unused bits (0x40, 0x80). Only
   available if we own the broadcaster (see F1) — not usable under Option B
   without diverging from the upstream format.

Three states, not two: **AWAKE** → **DARK** (inactivity, data still tracked) →
**NO SIGNAL** (nothing heard at all). Waking must be immediate on the first
active-cadence packet.

Note the scanner's own **scan duty cycle is the dominant power draw**, an order of
magnitude above the ~1.2% SPI duty. Blanking the panel without also relaxing the
scan window saves little. The sleep design must address both.

### D7 — Keyboard-side broadcaster must be cleanly extractable
All code that broadcasts to the display lives behind a single Kconfig gate,
in its own module/directory, with **zero edits to shared keyboard config paths**.
It must be reviewable as one self-contained diff and removable by flipping one
symbol.

**Why this matters beyond hygiene:** the central is the right half — already the
split central *and* the host BLE endpoint *and* the trackball owner, and the half
carrying the unresolved hang (see `crash-lag-investigation.md`). Any code added
there must be trivially bisectable out. Extra advertising also costs battery on
exactly the half with the heaviest radio load, so the active/idle interval choice
is a real power decision, not a default to accept.

**This tightens F1 considerably.** Option B reaches into ZMK's connectable
advertising and cannot be gated to zero effect; Option C is a separate advertising
set behind one Kconfig symbol. D7 favours C on architecture, independent of the
earlier reliability argument.

### D8 — Displays must associate with a specific keyboard
Multiple keyboards and multiple displays may be in radio range simultaneously
(office, desk with two builds, a friend's board). A display must show **one
chosen keyboard**, deterministically, and must not flap to a neighbour's.

Note the asymmetry: because this is broadcast, **one keyboard to many displays is
free** — any number of displays can watch the same keyboard with no cost to the
keyboard and no coordination. The problem is exactly one-directional: *which*
keyboard does a given display bind to.

Existing material in the payload:
- `keyboard_id[4]` (offset 19) — a hash of `hwinfo_get_device_id()`, i.e. stable
  per physical keyboard and independent of BLE address rotation.
- `channel` (offset 25) — upstream's explicit pairing knob, with matching logic
  `scanner_ch == 0 || scanner_ch >= 10 || kb_ch == 0 || scanner_ch == kb_ch`.

Constraints that shape the answer:
- The display has **no keys and no user button** (the XIAO exposes only reset), so
  any runtime pairing needs a mechanism we do not currently have.
- BLE addresses may rotate; binding on address alone is not stable. This is
  precisely why upstream matches on `keyboard_id` and rewrites the address in
  place when it changes.
- The broadcast is **unencrypted**. A channel is a filter, not a security
  boundary — anyone in range can read any keyboard's layer and battery. In a
  shared office that is a real, if minor, disclosure. Worth stating plainly rather
  than implying the channel protects anything.
- Our display is single-keyboard and landscape-only; we do not want upstream's
  3-slot multi-keyboard UI.

---

## Open forks (not yet ruled)

| # | Fork | Status |
|---|---|---|
| F1 | Broadcaster: ready-made kit (Option B) vs own ext-adv set (Option C) | **Deferred to Phase 3.** Leaning C — the kit reaches into ZMK's connectable advertising rather than opening a separate set, which couples the display feature to host pairing. Blocked on whether the kit's keyboard side even compiles on ZMK v0.2.1 (see R1). |
| ~~F2~~ | Orientation abstraction | **RULED** — compile-time Kconfig choice, two CI artifacts. See below. |

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

## D5 — Orientation: **LANDSCAPE ONLY** (ruling on F2)

**Owner decision, 2026-08-07: portrait is out of scope.** The display renders
native 160x68 and never rotates.

**What this removes from the build**, all of it:
- the custom rotating flush callback — the highest-risk item in Phase 4, with
  four independent ways to produce a subtly wrong screen (byte bit-order,
  `MONO01` polarity, the 8-byte I1 palette header, stride alignment)
- the orientation Kconfig `choice` and the `dispscan_layout` vtable
- the two-artifact `build.yaml` matrix — one firmware image, not two
- `layout_portrait.c` and the 68x160 composition entirely

**What this buys.** Landscape needs **zero rotation at any level** — stock Zephyr
glue, stock flush callback, stock rounder, ~2.7 KB of buffers. It is also the
better-reading layout: the layer name gets **112px (14 chars at unscii_8)** rather
than portrait's 68px/8-chars-across-two-lines, so a name like `NAVIGATION` fits on
one line with room to spare. Portrait's 16-char worst case would have consumed both
available lines with zero margin, resting on an unconfirmed font advance width.

Keep a **clean seam** at the composition layer (one module owns all
`lv_obj_align()` calls) so a future orientation is a contained change — but do not
build the abstraction now. Retrofitting portrait means reopening this design, and
that is the accepted trade.

The analysis below is retained because its findings still constrain the landscape
build — in particular the partial-update contract, the font ruling, and the
correction that SPI is not a bottleneck.

### Superseded analysis (portrait)

Derived blind from constraints, verified against LVGL `release/v9.3` and Zephyr
`v4.1-branch` sources.

### The three findings that decide it

**1. `lv_draw_sw_rotate` has no 1-bit case.** `src/draw/sw/lv_draw_sw_utils.c`
handles only `L8`, `RGB565`, `RGB888`, `ARGB8888`, `XRGB8888`. No `I1`. This is
*why* ZMK's nice_view widget uses L8 canvases — the comment "L8 is the smallest
type supported by sw_rotate" is literally true. Rotating via LVGL forces an **8x
memory inflation** over the 1bpp we actually need.

**2. LVGL 9.3 display-level rotation is unusable here.** `lv_display_set_rotation()`
only stores a field and recomputes resolution — it performs no pixel work. The
core render path rotates only under `lv_display_set_matrix_rotation()`, which
requires render mode `DIRECT` or `FULL`, but Zephyr's glue registers the display
as `PARTIAL`; and the matrix path routes through the SW transform code, which
also has no I1 case. It would fail **silently** — a correctly-laid-out scene
rendered into a mis-shaped buffer.

> Rotation must therefore happen either **above** LVGL (ZMK's canvas approach) or
> **below** it (a custom flush callback). There is no supported middle path at 1bpp.

**3. SPI is not the constraint — an earlier assumption in this project was wrong.**
A full 160x68 frame is `1 + 68*22 + 2 = 1499 bytes` = ~**12 ms** at 1 MHz =
**1.2% duty at 1 Hz**. Full-frame repaints are not a battery or correctness
problem. BLE scan duty cycle will dominate power by an order of magnitude. The
goal is *zero repaints in the steady state* because they are free to avoid — not
because they are expensive to do.

### The ruling

**Compile-time Kconfig `choice`, exposed through a runtime vtable, shipped as two
CI artifacts.**

Runtime switching is rejected on a decisive argument: **the device has no keys and
never accepts a connection.** There is no input channel through which a user could
ever flip a setting. It would be dead code behind an NVS write. Reflashing to
change orientation is a one-minute operation on a device you have physically
picked up and reoriented anyway.

- **Landscape: no rotation at any level.** Native 160x68, stock glue, stock flush
  cb. Zero incremental RAM, full partial-update support.
- **Portrait: rotate *below* LVGL** in a custom flush callback operating on 1bpp
  data. Render I1 natively into a 68x160 buffer; a hand-written bit-rotation
  blits into panel order.
- **Do not use ZMK's square-canvas approach.** Keep it documented as fallback only.

| Approach | RAM |
|---|---|
| Landscape, native | ~2.7 KB |
| Portrait, flush-level rotation | ~3.3 KB |
| Portrait, ZMK L8 canvases | **~21 KB** |

Beyond RAM, the canvas approach also loses LVGL layout (content must be composed
inside 68x68 tiles hard-positioned with magic offsets; nothing may straddle a
boundary) and loses partial redraw (any pixel invalidates a whole 68x68 region,
which the mono rounder widens to full panel width → the entire screen).

The larger win: **portrait and landscape become the same program** — same widgets,
same objects, same invalidation machinery. Only `(hor_res, ver_res)`, the
composition module, and one flush callback differ.

### Partial-update contract

`ls0xx.c` reports `SCREEN_INFO_X_ALIGNMENT_WIDTH` only, and `ls0xx_write()`
rejects any `desc->width != PANEL_WIDTH`. **The only partial granularity that
exists is a full-width horizontal band of scanlines.**

Consequence — and this is non-obvious: **portrait inverts the cost model.** After
rotation, logical x maps to panel *y*. So an element's *horizontal* extent
determines cost and its vertical extent is free. Since portrait is naturally a
stack of full-width rows, **every portrait update is a full frame** (~12 ms).
Accept it; start with render mode `FULL`.

In landscape the band structure is load-bearing, not cosmetic: keeping each
volatile field in its own horizontal band bounds its invalidation to ~10-22 rows.
Never place a *tall changing* object — it forces all 68 rows for a 1px change.

### Font ruling

At 1bpp there is **no antialiasing**; Montserrat 14's thin strokes drop out
entirely. Use **`lv_font_unscii_8`** (a bitmap font designed for 1bpp) for all
small text, and **Montserrat 18 only** for the single large layer digit.
68px portrait = **8 characters per line** at unscii_8 — the 16-char layer name
consumes exactly two lines with zero margin.

### Highest-risk step, to be isolated

The portrait rotating flush callback has four independent ways to produce a subtly
wrong screen: byte bit-order (inferred LSB-first from the *absence* of
`SCREEN_INFO_MONO_MSB_FIRST`), `MONO01` polarity, the 8-byte I1 palette header,
and stride alignment. Note LVGL reads I1 **MSB-first**, so there is a bit reversal
inside every byte. Validate with an asymmetric test pattern (draw an "F") before
wiring any real layout. Write it as an obviously-correct slow loop first;
optimise only once it is on screen.

### Build order

1. Data model + Kconfig skeleton (no hardware dependency)
2. Landscape end-to-end against a **fake data source** — validates fonts, widgets,
   and redraw masking before any BLE exists
3. BLE observer + packet decode; swap out the fake source
4. Staleness timer + no-signal screen
5. Portrait flush cb, **validated in isolation**
6. Portrait composition, reusing widgets unchanged
7. `build.yaml` → two artifacts

---

## Phase 4 working ledger

| Lane | Purpose | Status |
|---|---|---|
| research/build-facts | XIAO board name, keyless build idiom, observer Kconfig, `nice_view_spi`, CS polarity | **landed** → facts above |
| research/payload | Byte-level advertisement struct, scanner parse/filter, keyboard-side ZMK version floor (R1), widget reuse from `zmk-dongle-display` | running |
| design/orientation | Blind re-derivation of the orientation abstraction (F2) | running |
| author/scaffold | Shield plumbing slice — builds clean + placeholder screen. No BLE, no parsing | running |
