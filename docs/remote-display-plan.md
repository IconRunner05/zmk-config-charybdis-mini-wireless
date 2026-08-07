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
| R1 | Keyboard-side status-advertisement code may not support ZMK v0.2.1. Upstream states ZMK 0.3 support "via compatibility macros"; v0.2.1 is older. If it does not build, F1 resolves to C by force. |
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

## Phase 4 working ledger

| Lane | Purpose | Status |
|---|---|---|
| research/build-facts | XIAO board name on ZMK main, keyless ZMK build idiom, BLE observer Kconfig, `nice_view_spi` on a non-pro-micro board, CS polarity | dispatched |
| research/payload | Byte-level advertisement struct, scanner parse/filter, keyboard-side ZMK version floor (R1), widget reuse from `zmk-dongle-display` | dispatched |
| design/orientation | Blind re-derivation of the orientation abstraction (F2) | dispatched |
