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

### D10 — Sequencing: display first; keyboard soak stays pending
Offered the owner three orders (display-first / keyboard-first / both staggered).
**Owner chose display-first.** Phase 4 proceeds now; Phases 0–2 on the keyboard
branches are **not** started, and the Phase 2 soak clock has **not** begun.

**Consequence to keep visible:** Phase 3 (the broadcaster) is gated behind the
Phase 2 soak, and that soak is a week of wall-clock that nobody is spending yet.
The scanner can reach "renders live fake data" and even "decodes a real packet
from a bench transmitter" without it, but it cannot reach "shows this keyboard"
until the keyboard line is stabilised. R2 (the unrun BLE anchor fix `97b8436`)
and R3 (unpinned trackball driver) both remain open and untouched.

**Owner also reports the display will stay USB-tethered to the dev machine
throughout development.** This is load-bearing, not incidental:
- It gives a **USB CDC console** — `LOG_INF` over serial. A keyless, hostless,
  screen-only device otherwise has no output channel but the panel itself, so
  this is the difference between debugging the packet decoder and guessing at it.
- It **confirms D9 empirically** rather than by assumption: the scanner is on
  mains for the whole of Phase 4, so the scan-duty state machine stays deleted
  and 100% scan duty is simply correct for now.
- Reflash needs no reset-button double-tap dance.

It does **not** change D6: going dark on inactivity survives as an *honest-UX*
requirement (never show stale state), not a power one. See D9.

---

## Open forks (not yet ruled)

| # | Fork | Status |
|---|---|---|
| ~~F1~~ | Broadcaster: ready-made kit (B) vs own ext-adv set (C) | **RULED — B2: our own minimal broadcaster, legacy API, one Kconfig gate.** See below. |

### F1 — RULED: own minimal broadcaster on the legacy API (B2)

Owner ruled **B2** after the fabricated evidence base was rebuilt from scratch.

**Option C is dead, on two independent grounds.**

*Ground 1 — it is not isolated.* Upstream shipped ext-adv (`b387c31`, 2026-01-15,
to fix macOS/iOS pairing) and removed it on **2026-02-20 across four commits in
under two hours**, ending at *"Remove BT_EXT_ADV dependency - use legacy BLE API
only"*. Independently verified in `subsys/bluetooth/host/adv.c` at the exact
revision ZMK v0.2.1 pins: `bt_le_adv_start()` branches on
`IS_ENABLED(CONFIG_BT_EXT_ADV)`, and `adv_get_legacy()` switches from the static
`bt_dev.adv` to a slot allocated from `adv_pool[]` — **created on every start and
deleted on every stop.** So setting `BT_EXT_ADV=y` silently reroutes ZMK's *own*
advertising, with zero edits to ZMK. Option C's premise — "fully isolated" — is
true at the API level and false at the implementation level.

*Ground 2 — it cannot satisfy D7.* The Zephyr fix cherry-picks cleanly (PR #71611,
2 files, +40/−13, verified to apply to `zmkfirmware/zephyr` with zero conflicts —
the two patched files are byte-identical to upstream v3.5.0). But landing it means
forking Zephyr and repointing the west manifest. **That edit lives in a shared
config path, outside the Kconfig gate, and cannot be reverted by flipping a
symbol.** D7 rules it out regardless of radio behaviour.

**Why B2 over B1/B3.** B1 (adopt upstream wholesale) hands the riskiest behaviour —
the connectable proxy that owns keyboard pairability while disconnected — to code
we cannot bisect, carrying live split-keyboard bugs upstream is still patching.
B3 (piggyback only, `bt_le_adv_update_data()` and nothing else) has a documented
zero-interference property and is ~50 lines, but the keyboard only advertises while
**disconnected**, so the display freezes exactly while the keyboard is in use.
B2 keeps the risk in code we wrote, behind one symbol, in one reviewable diff.

**v0.2.1 is a better target than feared.** `event_manager.h` is **byte-identical**
to `main`. `ZMK_LISTENER`/`ZMK_SUBSCRIPTION` register via additive linker sections
(`KEEP(*(".event_subscription"))`), so a module never patches an app-side registry.
Every payload field is publicly reachable — **except caps-word**, which has no event
and no accessor at v0.2.1 *or* `main`. Upstream only gets it by substituting a
forked `behavior_caps_word.c`, and only in adapter builds; its keyboard-side
broadcaster never populates that bit. **Treat caps-word as unavailable.**

**Confidence note.** The *fact* of upstream's reversal is hard evidence (real
commits, read directly). The *mechanisms* in those commit messages are
agent-authored hypotheses validated by field symptoms, not radio traces — medium
confidence. What was verified independently in Zephyr and ZMK v0.2.1 source stands
on its own.

### Branch topology for the broadcaster — READ BEFORE STARTING

The broadcaster is **keyboard-side** and therefore **cannot live on
`display/scanner`** (D3: one repo, one `west.yml`, one ZMK revision). It forks from
the keyboard line (ZMK v0.2.1). `display/scanner` and the broadcaster branch never
merge in either direction.

**Consequence — the wire layout is duplicated.** The scanner's decoder
(`dispscan_packet.h`) and the broadcaster's encoder live on branches that can never
share a file. Two copies of the same 26-byte layout must be kept in sync by hand.
This is the real cost of D3 and it is not avoidable without a third shared repo.
Any change to the layout must be applied to both, and the decode self-test
(`dispscan_decode_test.c`) is the cheapest place to catch a drift.
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

## F1 — verified Zephyr blocker for Option C on ZMK v0.2.1

**Two concurrent advertising sets can hard-assert on Zephyr 3.5.**

Zephyr issue **#71608** — *"Bluetooth: Controller: Multiple Broadcaster asserts when
each of them overlap over time"* — fixed by **PR #71611**, which **merged 26 April
2024**. Zephyr **v3.5.0 released October 2023**, so the fix predates nothing: it is
**six months too late to be in v3.5.0**, and ZMK v0.2.1 pins Zephyr 3.5.
*(Independently verified: PR and issue numbers, merge date, and the release timeline.)*

Mechanism: when multiple advertising sets overlap in time, applying the random
advertising delay needs more ticker operation context than is available;
`ticker_update()` returns failure and the `LL_ASSERT` in `ticker_update_rand()`
fires → hard fault and reset. v3.5.0's `ull_adv.c` calls `ticker_update_rand()`
unconditionally on every adv event of every set; the fix gates it behind
`ticker_update_req == ticker_update_ack`.

**This is exactly the two-concurrent-set configuration Option C requires, on exactly
the version we run.**

Consequences:
- Option C on v0.2.1 is **conditional on cherry-picking PR #71611** onto the Zephyr
  fork. Reported as small and confined to `ull_adv.c` — **unverified**, needs costing.
- Adding a known-unfixed controller assertion to the half already under hang
  investigation violates D2's one-variable-at-a-time discipline. An assert produces
  a *reset*, which is a different signature from the no-banner hang — but
  introducing it mid-investigation is exactly the mistake D2 exists to prevent.
- Phase 3 stays behind the Phase 2 soak, as planned.

**Related footgun:** `ull_adv.c` initialises `lll.tx_pwr_lvl = RADIO_TXP_DEFAULT`
only `#if !defined(CONFIG_BT_CTLR_ADV_EXT)`. So enabling
`BT_CTLR_TX_PWR_DYNAMIC_CONTROL` alongside `BT_CTLR_ADV_EXT` **silently drops
advertising from +8 dBm to 0 dBm** until a VS write is issued per set. Also
`BT_CTLR_ADV_EXT=y` with `BT_EXT_ADV=n` is a broken combination — the controller
answers Command Disallowed to legacy adv commands. Flip them together.

RAM cost of ext-adv estimated at **~0.85 kB** with `CONFIG_BT_CTLR_ADV_AUX_SET=0`
(~1.3 kB without). Flash cost unmeasured.

**First move when Phase 3 opens:** enable `BT_EXT_ADV=y` *alone*, with no beacon
code, and confirm the keyboard still pairs, reconnects, and holds its split link.
That isolates the controller question from the feature.

### Research-integrity note — read before trusting the F1 evidence base

One research lane **fabricated evidence** in an internal report: commit SHAs, dates,
commit-message quotes, upstream issue quotations, and source line-number citations,
all invented, on the questions of *why upstream abandoned ext-adv* and *the ZMK
v0.2.1 event/module API surface*. It self-reported and retracted.

**None of that material reached this document** — the retracted claims were never
recorded. But two consequences stand:

1. **We currently have no real evidence on why upstream retreated from ext-adv.**
   That was to be a load-bearing input to F1. It must be re-investigated from
   scratch.
2. **The ZMK v0.2.1 event/module surface for D7's extractable module is unverified.**
   Re-derive before designing against it.

The Zephyr blocker above survived because it was **re-verified independently** —
against the PR page and the release timeline — rather than taken on report.
Apply the same standard to anything else that decides hardware behaviour on the
half carrying the unresolved hang.

---

## D6 resolution — display sleep and the power budget

### Three findings that reshape the problem

**F-A. `display_blanking_on()` is a NO-OP on the nice!view.** `ls0xx_blanking_on/off`
compile only under `#if DT_INST_NODE_HAS_PROP(0, disp_en_gpios)`; otherwise they
`LOG_WRN("Unsupported"); return -ENOTSUP;`. ZMK's `nice_view.overlay` declares no
`disp-en-gpios` and no `extcomin-gpios` — the panel exposes only CS/MOSI/SCK/VCC/GND.

So `CONFIG_ZMK_DISPLAY_BLANK_ON_IDLE=y` would **not** darken this panel. It only
stops the tick timer, freezing the last frame on a display that holds it
indefinitely — **permanently-displayed stale data**, the worst possible outcome for
a status readout. Keeping it `n` is correct for a second reason beyond the default.

> **"Dark" must be implemented by drawing a blank frame, not by the blanking API.**

**F-B. There is no public API to feed ZMK's activity subsystem.** `note_activity()`
is `static`; `zmk/activity.h` exposes only `zmk_activity_get_state()`. Calling the
non-static `set_state()` does not touch `activity_last_uptime`, so the 1 Hz handler
re-latches IDLE within a second. **Own the state machine in the scanner module** —
we need four states where ZMK has three, ZMK's only consumer does the wrong thing
here (F-A), and our states must also drive scan parameters, which ZMK knows nothing
about.

**F-C. Wake latency is set by the *broadcaster's* advertising interval, not the
scanner.** At a 1 Hz beacon you get one chance per second, so sub-second wake
demands ~100% scan duty — the entire power budget.

### Power budget — the assumption was right, and understated

| Rank | Consumer | Avg current | Share |
|---|---|---|---|
| 1 | **BLE scan RX @ 100% duty** | **~4800 µA** | **~97%** |
| 2 | LVGL 10 ms tick | ~20-80 µA | ~1% |
| 3 | ls0xx VCOM thread @ 33 ms | ~30-50 µA | ~1% |
| 4 | Frame writes @ 1 Hz | ~30 µA | <1% |
| 5 | Panel static | ~5-20 µA | <1% |

A 100%-duty scan is not "an order of magnitude" above the rest — it is **~40x the
sum of everything else combined**.

> **Blanking the panel without relaxing scan duty is not a power feature.** It saves
> 1-2% — on a 500 mAh cell, 4.3 days becomes 4.4 days. Do it for honest UX (never
> show stale state), not for battery.
>
> **Relaxing scan duty is worth 10-20x alone.** 100% ≈ 4.3 days; 20% ≈ 19 days;
> 5% ≈ 59 days. Cut the scan first — only then does blanking earn its keep.

### The advertising-interval knob — a power TRANSFER, not a free win

**Corrected 2026-08-07.** An earlier revision of this section called this the
"highest-leverage knob, and it is free." That was wrong in two ways and the
wording was actively misleading. Restated properly:

**Direction, stated plainly:** shortening the advertising interval from 1000 ms to
200 ms means the keyboard advertises **five times more often** — 5 beacons/sec
instead of 1. It is an *increase* in keyboard radio activity, not a reduction.

**Why that helps the scanner** — the counterintuitive part. A scanner catches a
beacon only if one lands inside a scan window, so with random relative phase:

```
wake latency ≈ advertising_interval / scan_duty
```

For a fixed ~1 s wake target: a 1000 ms beacon forces **100%** scan duty (one
chance per second, cannot afford to miss it); a 200 ms beacon allows **20%**; a
100 ms beacon allows **10%**. More beacons means more chances, so the listener can
sleep through most of them and still catch one in time.

**The trade, both sides:**

| | 1000 ms beacon | 200 ms beacon | Delta |
|---|---|---|---|
| Central (keyboard) | ~10 µA | ~50 µA | **+40 µA** |
| Scanner radio | ~4800 µA | ~960 µA | **−3840 µA** |

So it buys ~4x the scanner's battery life at a cost of roughly **10-20% of the
central's**, against a typical few-hundred-µA ZMK baseline on a small split LiPo.

**This is not free, and its correct direction is not obvious.** Per D7 the owner
has explicitly flagged central power as a concern, and the central is already the
heaviest-radio half and the one carrying the unresolved hang. The scanner, by
contrast, is a desk device that can be USB-powered or carry a far larger cell.

`CONFIG_ZMK_STATUS_ADV_ACTIVE_INTERVAL_MS` is an **existing upstream Kconfig**, so
either direction is configuration rather than a fork. *That* — and only that — was
the sense in which the earlier "free" claim was true.

### D9 — RULED: display is USB-powered; minimise central draw

**Owner decision, 2026-08-07.** The display will be plugged into USB. Scanner power
is a non-goal. **Optimise for the central half's battery.**

**Setting: keep `ACTIVE_INTERVAL_MS` at ~1000 ms. Do not go to 200 ms, and do not
go much slower either.** The cost curve flattens hard:

| Active interval | Central cost @ 0 dBm | @ +8 dBm (our setting) | Wake latency @ 100% scan |
|---|---|---|---|
| 200 ms | ~50 µA | ~95 µA | 0.2 s |
| **1000 ms** | **~10 µA** | **~19 µA** | **1.0 s** |
| 2000 ms | ~5 µA | ~10 µA | 2.0 s |
| 5000 ms | ~2 µA | ~4 µA | 5.0 s |

Against a typical ZMK split-central baseline of a few hundred µA, 1 Hz costs
roughly **2-5%** — genuinely small. Slowing to 5 s saves ~15 µA (low single-digit
percent) and makes the display feel broken. **1 Hz is the knee of the curve.**

The idle interval (30 s) costs ~0.3 µA and is effectively free — leave it.

**Note on TX power.** The keyboard sets `CONFIG_BT_CTLR_TX_PWR_PLUS_8=y` for host
range, and under Option B the beacon inherits it — roughly **doubling** the
beacon's cost (nRF52840 TX is ~16 mA at +8 dBm vs ~4.8 mA at 0 dBm). A desk-range
display needs nothing like +8 dBm. Per-advertising-set TX power exists only with
extended advertising, i.e. Option C — but that carries the verified Zephyr 3.5
assert blocker, and the saving is ~10 µA. **Not worth pursuing for power alone.**

### Consequence: the scanner power state machine collapses

D6's four-state design existed to manage *scanner* battery. With the display on
USB, most of it is unnecessary:

- **Scan at 100% duty, always.** No DARK-state duty reduction, no
  `SLOW_INTERVAL_1` baseline, **no periodic census bursts**, no `bt_le_scan_stop/start`
  transitions and their blind windows.
- **NO-SIGNAL detection becomes fast and reliable** rather than a 10-15 minute
  statistical inference. The awkward "reducing DARK duty forces the NO-SIGNAL
  timeout way up" problem simply disappears.
- **Activity inference gets easy.** At 100% duty essentially every beacon is
  caught, so **S1 (advertising cadence) is now a reliable signal** — the k-of-window
  test and its ~10 s evidence lag are no longer needed to disambiguate "idle" from
  "missed it".

**This also removes D6's dependency on owning the broadcaster.** S4 (a dedicated
ACTIVE bit in `status_flags` 0x40) was needed only because cadence inference
degraded at low scan duty. At 100% duty S1 suffices, so **D6 no longer pushes F1
toward Option C.** Combined with the D6 finding that configuring the broadcaster
beats owning it for power, F1 now rests on **D7 extractability and reliability
alone**.

### What survives of D6

Going dark on inactivity is still wanted — but its justification is now **honest UX
only**: never leave stale keyboard state displayed. It is not a power feature
(saves 1-2% of a mains-powered device) and it is not a longevity feature (this
panel cannot burn in; VCOM handles DC bias unconditionally).

Two amendments to the earlier state machine:
- **Drop the "USB present pins AWAKE" rule.** It was a battery heuristic and would
  now defeat the requirement entirely, since USB is always present.
- Keep AWAKE/DARK/NO-SIGNAL as *display* states driving only what is drawn. They no
  longer drive scan parameters, because those are now constant.

**Refines F1:** owning the broadcaster is worth *one bit* (a dedicated ACTIVE flag
in `status_flags` 0x40) and one line of code. *Configuring* the broadcaster — which
Option B already permits — is worth 5-10x the scanner's battery. These are
separable, and the second is the one that matters. The power argument therefore
favours C only weakly. Decide F1 on reliability and D7 extractability.

### Activity signals — what works without owning the broadcaster

| Signal | Needs broadcaster? | Reliability |
|---|---|---|
| **S1** advertising cadence | No | Degrades exactly where needed — at 20% duty a 5 s gap is ambiguous. Use as a **k-of-window** test, never a single-gap test. |
| **S2** payload delta | No | Perfect positive signal, but **sparse** — typing monotone prose with WPM off yields a byte-identical payload indefinitely. |
| **S3** `wpm_value != 0` | No, if keyboard has `ZMK_WPM=y` | Lags by seconds. UNCONFIRMED whether upstream selects it. |
| **S4** dedicated ACTIVE bit (`status_flags` 0x40) | **Yes** | **Perfect** — one packet, unambiguous, works at any scan duty. |

Make S4 **backwards-compatible** (bit clear on old firmware → fall back to S1+S2)
so the scanner firmware is identical either side of the F1 decision.

**Constraint to state plainly: S1 alone cannot give a sub-5-second wake at reduced
duty.** Evidence takes ~10 s to accumulate.

### Four states, not three

**AWAKE** (panel live, scan 30/30 = 100%) → **DARK** (blank frame drawn, tick
stopped, state retained in RAM so wake redraw is instant; scan 30/150 = 20%) →
**NO SIGNAL** (minimal marker; slow baseline **plus a periodic 100%-duty census
burst**). USB present pins AWAKE.

**Non-obvious: reducing DARK scan duty forces the NO-SIGNAL timeout way up.** At
20% duty you must wait 10-15 min before declaring the keyboard gone. And a pure
slow scan can *never* re-detect a 30 s idle beacon (p95 ≈ 2.8 hours) — hence the
census burst (~35 s at full duty every 5-15 min).

**`T_dark` must be >= the keyboard's `ACTIVITY_TIMEOUT_MS` + margin** (default 90 s).
Shorter and you go dark while the keyboard still advertises actively — burning
fast-scan power on an invisible screen, then flickering bright on the next keypress.

**Never enable `CONFIG_ZMK_SLEEP`** — this device has no keys and no
`zmk,soft-off-wakeup-sources`, so `sys_poweroff()` is a one-way trip recoverable
only by the reset button.

### Rendering "dark" — order is inverted from ZMK's

ZMK's `blank_display_cb` stops the tick *then* blanks. Because blanking is a no-op
here, we must: (1) load the blank screen, (2) **let one LVGL frame actually flush**
(~12 ms full-panel write), (3) *then* stop the timer. Never touch the VCOM thread.
Fill **black** — white reads as a dead panel.

### Burn-in — premise corrected
**Not a risk.** A Sharp Memory LCD is reflective TN with a 1-bit SRAM cell per
pixel; it has no organic emitter to age differentially. The real hazard is DC bias
from failed VCOM inversion — and that is handled **unconditionally** by the
driver's `while(1)` VCOM thread, which nothing in ZMK can stop. Safe to leave
blanked for hours. The justification for going dark is **power and honest UX, not
panel longevity**.

Corollary: that 30 Hz VCOM thread is a hard floor on idle current — this device
cannot reach deep low-power without cutting panel VDD in hardware.

---

## D8 resolution — binding by `keyboard_id` allowlist

**Primary: a compile-time `keyboard_id` allowlist (N=2..3 entries) enforced as a
hard reject in the scan callback. Fallback: the unbound state of that same
mechanism** — an empty allowlist puts the display in *discovery mode*, rendering
the `keyboard_id` of the strongest keyboard it hears. Setup and graceful
degradation are one mechanism, not two.

Setup UX: flash default firmware → read 8 hex digits off the screen → paste into
the shield `.conf` → rebuild. No USB, no host tooling, no shell. The landscape
layout has 112px = 14 chars at unscii_8, so 8 hex digits fit with a label.

**Why this and not the alternatives:** a non-allowlisted ID never reaches display
state, so the only possible wrong output is `NO SIGNAL` — loud and correct.
Every other scheme can fail *silently* by rendering a stranger's data.

### Do not use upstream's channel matching — it is broken

`src/status_scanner.c:153`:
```c
bool channel_match = (scanner_channel == 0 || scanner_channel >= 10 ||
                      keyboard_channel == 0 || scanner_channel == keyboard_channel);
```
Two independent defects:
1. **`keyboard_channel == 0` is a wildcard held by the *keyboard*.** A colleague
   running defaults (channel 0) is accepted by **every** scanner regardless of how
   yours is set. Setting a channel buys nothing against the exact scenario
   channels exist to solve.
2. **`scanner_channel >= 10` is silently promiscuous.** Pick "42" thinking it is a
   private channel and you get a wide-open scanner.

We own the scanner, so: **strict equality only**, no wildcards, no escape hatch.
Keep channel as an optional coarse pre-filter; never as the identity.

### Why `keyboard_id` is the right key
Derived from FICR `DEVICEID` via `hwinfo_get_device_id()`, so it survives reflash,
rename, bootloader update, `settings_reset`, profile clear, and BLE address
rotation. The name-hash fallback is **dead code on nRF52840** — the driver
unconditionally returns 8 bytes — so "a rename breaks the binding" does not apply
to this hardware. Only swapping the physical MCU changes it, which fails loudly.

Collision probability across 5 devices: ~2.3 x 10^-9. Non-issue.

### Three liveness states — distinct from D6's DARK
- **LIVE** — last matching packet < 5 s
- **IDLE** — 5-90 s. Keep rendering last-known values with a staleness marker.
  This window must span the ~30 s idle cadence or a keyboard merely sitting there
  reads as gone.
- **NO SIGNAL** — > 90 s (3x idle interval, so one dropped advert cannot trigger it)

**Never zero out last-known values** — a battery reading of 0% is worse than a
stale one. And **DARK (D6 inactivity) must look different from NO SIGNAL** — a
user must never mistake "display slept" for "keyboard died".

### Two keyboards, one display
Allowlist holds N entries. Active = most recently heard, with hysteresis: switch
only when the challenger has been more recent for >=3 consecutive packets **and**
the incumbent has been silent >=10 s. Prefer **advertising cadence** as the
tiebreak over RSSI — the board you are typing on is the one advertising at the
active rate, which is a semantic signal rather than a physical proxy. With N>1,
render *which* keyboard is shown or the user cannot tell.

### One keyboard, two displays — free, and passive scanning makes it strictly free
BLE broadcast is connectionless; N observers impose no state on the transmitter.
Under **passive** scanning there is literally zero keyboard-side cost. Note
upstream uses **active** scanning, under which each scanner costs the keyboard a
SCAN_RSP transmission per advertisement — an additional, previously unstated
argument for the passive ruling.

### Byte-order trap — must be handled explicitly
The broadcaster does `memcpy(keyboard_id, &id_hash, 4)` on a little-endian MCU, so
wire bytes are LSB-first, while the keyboard's own debug log prints `id_hash` as
`%08X` (MSB-first text). **Define the canonical form as the uint32 as printed**,
have the discovery screen print that same uint32, and reconstruct with
`sys_get_le32()` before comparing. Get this inconsistent and the user pastes a
byte-swapped ID, sees `NO SIGNAL`, and has nothing to debug against.

### Privacy — accept it, add nothing
**Leaked:** layer index + 4-char name, both battery levels, profile slot,
USB/BLE/bonded/caps flags, modifier state (class only), WPM, and a stable 4-byte
device ID. **Not leaked: keystrokes, key codes, text, or key positions.** WPM is an
aggregate rate. Worth stating plainly — "my keyboard is broadcasting" invites a
worse assumption than the reality.

The real exposure is **presence and tracking, not content**: an observer learns
whether you are at your desk and typing. And `keyboard_id` is a permanent,
non-rotating identifier that partially defeats BLE address privacy — which is
precisely the property that makes it a good binding key.

Mitigation rejected as disproportionate. A "shared secret" in the two spare
`status_flags` bits gives **four possible values** — theatre, and it burns bits
D6 wants for the activity flag. The tracking concern is real but marginal: the
keyboard already emits a fixed connectable advert with a fixed device name for
host pairing, an equally good tracker. Encrypting the payload while the name
beacon stays in the clear achieves nothing. If the office case ever matters, the
correct control is slowing the idle cadence or gating broadcast on at-desk, which
cuts exposure *and* battery — not encryption.

Spoofing is trivial and cosmetic: no control path, nothing actuates.

### Impact on F1
**None — and that is the finding.** The entire primary scheme works against an
unmodified upstream broadcaster, because `keyboard_id` is already in the payload.
F1 should be decided on the reliability and D7-extractability arguments alone.

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

## CONFIRMED ON HARDWARE — LVGL colour constants are inverted on this panel

First flash, 2026-08-07. **`lv_color_white()` renders BLACK on the nice!view;
`lv_color_black()` renders WHITE.** Anywhere this document says "fill black",
the code must use `lv_color_white()`.

Chain, verified in the pinned trees:
1. LVGL at `LV_COLOR_FORMAT_I1` maps white → bit 1, black → bit 0.
2. `ls0xx` reports `PIXEL_FORMAT_MONO01`; Zephyr's `lvgl_display_mono.c`
   `set_px_at_pos()` **clears** the destination bit for a *set* source pixel in
   the MONO01 case (`*buf &= ~BIT(bit)`), from a `0xFF`-filled buffer.
3. On a Sharp memory LCD a 0 bit is a **black** pixel.

ZMK encodes the same inversion, which confirms it is expected rather than a
local misconfiguration: `nice_view/widgets/util.h` defines `LVGL_FOREGROUND` as
`lv_color_black()` in the non-inverted case, and `util.c` passes it to a
descriptor named **`rect_white_dsc`** — named for what appears on the glass, not
for the constant. The stock nice!view look is therefore light content on a dark
field, which is what this display produces.

**This bit us once already.** `COL_DARK` was `lv_color_black()`, which would have
lit the panel *fully white* — maximum brightness — for the one state whose
purpose is to extinguish it, exactly inverting the "white reads as a dead panel"
rationale below. Fixed in `147f107`; DARK confirmed fully black on hardware.

Because DARK's panel-black equals the normal background, **DARK is the ordinary
field with every label hidden**, not a separately-coloured frame.

---

## Render-slice review round — verified outcomes

Three findings from the adversarial seat rested on inferences about files that were
not vendored in this repo at the time. A west workspace now exists, so all three
were checked **against the actual source**. Recording the results because two of
them correct statements this document previously asserted as verified fact.

### VCOM is driven unconditionally — but not for the reason stated above

This document claimed both that the panel exposes **no `extcomin-gpios`** (true) and
that the VCOM thread runs unconditionally (true) — without reconciling them. They
reconcile because there are **two** VCOM mechanisms, and the nice!view uses the
second one:

```c
/* zephyr/drivers/display/ls0xx.c:60 */
#if DT_INST_PROP(0, serial_vcom_inversion) || DT_INST_NODE_HAS_PROP(0, extcomin_gpios)
#define USE_VCOM_THREAD true
#endif
```

`nice_view.overlay` declares `serial-vcom-inversion;` and `serial-vcom-interval = <33>`,
satisfying the **first** disjunct. The `while(1)` thread therefore exists and takes
its `#elif serial_vcom_inversion` branch, sending a 2-byte empty command every 33 ms
purely to flip `LS0XX_BIT_VCOM`. This is independent of application writes.

**Consequence:** DARK — which produces zero application SPI traffic for its whole
duration — carries **no DC-bias risk**. "Safe to leave blanked for hours" stands.
Do not add a periodic write "for VCOM"; the driver already does it.

### The band layout is NOT what satisfies the ls0xx width constraint

Previously implied. It is wrong, and the real mechanism is unconditional:

- `ls0xx_get_capabilities()` reports `current_pixel_format = PIXEL_FORMAT_MONO01`
  and `screen_info = SCREEN_INFO_X_ALIGNMENT_WIDTH`.
- `zephyr/modules/lvgl/lvgl_display.c` registers `lvgl_rounder_cb_mono` on
  `LV_EVENT_INVALIDATE_AREA` for the MONO01/MONO10 case.
- `lvgl_rounder_cb_mono` sees `SCREEN_INFO_X_ALIGNMENT_WIDTH` and does
  `area->x1 = 0; area->x2 = cap.x_resolution - 1`.

So **every** invalidation is widened to the full 160 px before it reaches the flush
callback, and `ls0xx_write`'s `desc->width != LS0XX_PANEL_WIDTH` rejection can never
fire. The horizontal band layout bounds only the invalidation **height** — still
worth having, but for bandwidth, not legality.

### No duplicate `zmk_display_status_screen()` from `nice-view-gem`

`config/west.yml` does pull `m165437/nice-view-gem`, which defines its own
`zmk_display_status_screen()`. It lives at
`nice-view-gem/boards/shields/nice_view_gem/custom_status_screen.c` and compiles only
when **that shield** is in the shield list. We build `dispscan nice_view`. Confirmed
empirically: the image links. (Four definitions exist in the workspace overall —
ZMK's built-in, ZMK's `nice_view` shield, `corneish_zen`, and `nice_view_gem` — and
all four are excluded by shield selection or Kconfig.)

### Deferred to the BLE-observer slice — do not lose these

Raised by review, not fixable in a slice with no radio. Each is a real trap for the
next slice:

1. **The seam is push-only, so `NO_SIGNAL` is unreachable exactly when it matters.**
   The natural observer shape is *scan callback → decode → update*. Under it, a dead
   keyboard means the observer simply stops being called, the renderer is never told,
   and the panel holds a live-looking AWAKE screen with plausible values forever —
   verbatim the failure this document exists to prevent. **The observer slice must
   add a free-running timer that pushes NO_SIGNAL on silence.** Nothing in the
   current header tells its author that.
2. **D6's DARK and the liveness triple are orthogonal axes, and the struct has one.**
   LIVE / IDLE / NO-SIGNAL (staleness) is not the same axis as AWAKE / DARK
   (inactivity). Keyboard idle 60 s with beacons still arriving at the 30 s cadence is
   *IDLE with a staleness marker* on one axis and *heading toward DARK* on the other.
   There is currently nowhere to put a staleness marker and no spare object in bands
   A/D/E. Resolve the model before writing the observer.
3. **The battery L/R swap is a second hidden coupling.** Decoding trap #3 is "the
   decoder's job", but the wire carries no field saying which side the central is —
   it depends on the keyboard's `CONFIG_ZMK_STATUS_ADV_CENTRAL_SIDE`. The scanner
   must hardcode a keyboard-side Kconfig value. Flipping the keyboard's central to
   LEFT silently swaps the halves on the display with no wire signal. **D3's "the
   only contract is the byte layout" is therefore not strictly true** — say so
   plainly rather than discovering it later.
4. **No RSSI in the model.** D8's discovery mode ("show the strongest keyboard") and
   the two-keyboard hysteresis rule both need it. Add it when D8 lands.
5. **`DISPSCAN_PROFILE_MAX 4`** hardcodes ZMK's default `ZMK_BLE_PROFILE_COUNT` into
   the scanner. A keyboard built with more profiles renders `BT?` for valid slots.
6. **Activity inference belongs in its own unit.** Decoding 26 bytes and inferring
   keyboard state are two jobs; do not fuse them into the observer file.

---

## Phase 4 working ledger

| Lane | Purpose | Status |
|---|---|---|
| research/build-facts | XIAO board name, keyless build idiom, observer Kconfig, `nice_view_spi`, CS polarity | **landed** → facts above |
| research/payload | Byte-level advertisement struct, scanner parse/filter, keyboard-side ZMK version floor (R1), widget reuse from `zmk-dongle-display` | running |
| design/orientation | Blind re-derivation of the orientation abstraction (F2) | running |
| author/scaffold | Shield plumbing slice — builds clean + placeholder screen. No BLE, no parsing | running |
