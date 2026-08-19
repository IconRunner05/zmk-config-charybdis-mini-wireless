# =============================================================================
# ZMK Charybdis Mini Wireless — Local Build
# =============================================================================
#
# Uses the exact same Docker image as GitHub Actions CI for bit-for-bit parity:
#   zmkfirmware/zmk-build-arm:stable  (ZMK v0.2.1 / Zephyr v3.5.0+zmk-fixes)
#
# Usage:
#   make init        First-time setup: pull image, west init + update + export
#   make update      Re-run west update after changing config/west.yml
#   make build       Build all three firmware targets
#   make left        Build left half only  (charybdis_left)
#   make right       Build right half only (charybdis_right + ZMK Studio)
#   make reset       Build settings_reset flasher only
#   make pristine    Delete build caches inside the Docker volume
#   make clean       Full teardown: remove Docker volume + staging dirs
#   make firmware    List built .uf2 files
#   make help        Show this message
#
# WHY A NAMED DOCKER VOLUME?
#   Docker Desktop on macOS uses VirtioFS to share host paths into the Linux VM.
#   Git's pack-file inflate operations (used by `west update`) corrupt silently
#   when writing to VirtioFS bind-mounts — producing "unknown compression method"
#   fatal errors. Named Docker volumes live entirely inside Docker's Linux VM
#   (no VirtioFS in the path), so all git I/O is native and reliable.
#
# Volume layout:
#   zmk-workspace       Named Docker volume — west workspace (zmk, zephyr, modules)
#   ~/Docker/zmk-config Bind-mount staging for config/ (rsync'd from the repo)
#   ~/Docker/zmk-fw     Bind-mount staging for .uf2 output (space-free host path)
# =============================================================================

SHELL := /bin/bash
.DEFAULT_GOAL := help

# ─── Keyboard knobs ─────────────────────────────────────────────────────────
# Board, shields and derived .uf2 names live in keyboard.mk at the repo root.
# (Device name is in config/charybdis.conf — see keyboard.mk for why.)
include $(dir $(lastword $(MAKEFILE_LIST)))keyboard.mk

# ─── Paths ────────────────────────────────────────────────────────────────────

# Named Docker volume for the west workspace.
# All git/west operations happen inside Docker's Linux VM — no VirtioFS.
WORKSPACE_VOL  := zmk-workspace

# Absolute path to this repo (may contain spaces — never passed to Docker -v)
REPO_ROOT      := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

# config/ source in the repo (may be on iCloud/spaced path — never Docker -v)
CONFIG_PATH    := $(REPO_ROOT)/config

# Staging dirs on the host: space-free, Docker bind-mount safe
CONFIG_STAGE   := $(HOME)/Docker/zmk-config
FIRMWARE_STAGE := $(HOME)/Docker/zmk-fw

# Final .uf2 output directory inside the repo (gitignored)
FIRMWARE_DIR   := $(REPO_ROOT)/firmware

# Sentinel: touched after successful west init so build guards work without
# spinning up a container just to check workspace state.
SENTINEL       := $(HOME)/Docker/.zmk-initialized

# ─── Docker ───────────────────────────────────────────────────────────────────

ZMK_IMAGE := zmkfirmware/zmk-build-arm:stable

# West operations (init / update / zephyr-export)
DOCKER_WEST = docker run --rm \
	-v "$(WORKSPACE_VOL):/workspace" \
	-v "$(CONFIG_STAGE):/workspace/config" \
	-e HOME=/workspace \
	-w /workspace \
	$(ZMK_IMAGE)

# Build operations (also mounts firmware staging dir for artifact extraction)
DOCKER_BUILD = docker run --rm \
	-v "$(WORKSPACE_VOL):/workspace" \
	-v "$(CONFIG_STAGE):/workspace/config" \
	-v "$(FIRMWARE_STAGE):/firmware" \
	-e HOME=/workspace \
	-w /workspace \
	$(ZMK_IMAGE)

# Lightweight copy helper (alpine) — extracts .uf2 from volume to firmware stage
DOCKER_CP = docker run --rm \
	-v "$(WORKSPACE_VOL):/workspace:ro" \
	-v "$(FIRMWARE_STAGE):/firmware" \
	alpine

# ─── Helpers ──────────────────────────────────────────────────────────────────

define sync_config
	@echo "→ Syncing config → $(CONFIG_STAGE)"
	@mkdir -p "$(CONFIG_STAGE)"
	@rsync -a --delete "$(CONFIG_PATH)/" "$(CONFIG_STAGE)/"
endef

define check_init
	@if [ ! -f "$(SENTINEL)" ]; then \
		echo ""; \
		echo "✗ West workspace not initialized. Run: make init"; \
		echo ""; \
		exit 1; \
	fi
endef

# =============================================================================
# Targets
# =============================================================================

.PHONY: help init update build left right reset pristine clean firmware \
        dispscan dispscan-init dispscan-fake

help: ## Show this help
	@echo ""
	@echo "  ZMK Charybdis Mini Wireless — Local Build"
	@echo "  ==========================================="
	@grep -hE '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) | \
		awk 'BEGIN {FS = ":.*?## "}; {printf "  \033[36m%-12s\033[0m %s\n", $$1, $$2}'
	@echo ""
	@echo "  Volume    : $(WORKSPACE_VOL)  (docker volume)"
	@echo "  Firmware  : $(FIRMWARE_DIR)"
	@echo "  Board     : $(BOARD)  (shields: $(LEFT_SHIELD), $(RIGHT_SHIELD), $(RESET_SHIELD))"
	@echo "  Image     : $(ZMK_IMAGE)"
	@echo ""

# ─── Workspace Setup ──────────────────────────────────────────────────────────

init: ## First-time setup: create volume, west init + update + zephyr-export
	@echo ""
	@echo "→ Creating Docker volume ($(WORKSPACE_VOL))"
	@docker volume create "$(WORKSPACE_VOL)" > /dev/null
	@echo ""
	@echo "→ Pulling Docker image ($(ZMK_IMAGE))"
	docker pull $(ZMK_IMAGE)
	@echo ""
	$(call sync_config)
	@echo ""
	@if [ ! -f "$(SENTINEL)" ]; then \
		echo "→ Running west init (manifest: config/west.yml)"; \
		$(DOCKER_WEST) west init -l /workspace/config; \
	else \
		echo "→ West already initialized, skipping west init"; \
	fi
	@echo ""
	@echo "→ Running west update (~1–2 GB download on first run, please wait)"
	$(DOCKER_WEST) west update
	@echo ""
	@echo "→ Running west zephyr-export"
	$(DOCKER_WEST) west zephyr-export
	@touch "$(SENTINEL)"
	@echo ""
	@echo "✓ Workspace initialized. Run 'make build' to compile firmware."
	@echo ""

update: ## Pull latest module changes (run after editing config/west.yml)
	$(call check_init)
	$(call sync_config)
	@echo ""
	@echo "→ Running west update"
	$(DOCKER_WEST) west update
	@echo ""
	@echo "→ Running west zephyr-export"
	$(DOCKER_WEST) west zephyr-export
	@echo ""
	@echo "✓ Modules updated."
	@echo ""

# ─── Build Targets ────────────────────────────────────────────────────────────

build: left right reset ## Build all three firmware targets
	@echo ""
	@echo "✓ All targets built."
	@$(MAKE) --no-print-directory firmware
	@echo ""

left: ## Build left half  (LEFT_SHIELD → LEFT_UF2, see keyboard.mk)
	$(call check_init)
	$(call sync_config)
	@echo ""
	@echo "→ Building $(LEFT_SHIELD)"
	@mkdir -p "$(FIRMWARE_STAGE)" "$(FIRMWARE_DIR)"
	$(DOCKER_BUILD) west build \
		-s /workspace/zmk/app \
		-d /workspace/build/$(LEFT_SHIELD) \
		-b $(BUILD_BOARD) \
		-- \
		-DZMK_CONFIG=/workspace/config \
		-DSHIELD="$(LEFT_SHIELD)"
	$(DOCKER_CP) cp /workspace/build/$(LEFT_SHIELD)/zephyr/zmk.uf2 \
		/firmware/$(LEFT_UF2)
	@cp "$(FIRMWARE_STAGE)/$(LEFT_UF2)" \
		"$(FIRMWARE_DIR)/$(LEFT_UF2)"
	@echo "✓ Left → $(FIRMWARE_DIR)/$(LEFT_UF2)"
	@echo ""

right: ## Build right half (RIGHT_SHIELD + ZMK Studio → RIGHT_UF2, see keyboard.mk)
	$(call check_init)
	$(call sync_config)
	@echo ""
	@echo "→ Building $(RIGHT_SHIELD) (with studio-rpc-usb-uart + ZMK Studio)"
	@mkdir -p "$(FIRMWARE_STAGE)" "$(FIRMWARE_DIR)"
	$(DOCKER_BUILD) west build \
		-s /workspace/zmk/app \
		-d /workspace/build/$(RIGHT_SHIELD) \
		-b $(BUILD_BOARD) \
		-S "studio-rpc-usb-uart" \
		-- \
		-DZMK_CONFIG=/workspace/config \
		-DSHIELD="$(RIGHT_SHIELD)" \
		-DCONFIG_ZMK_STUDIO=y
	$(DOCKER_CP) cp /workspace/build/$(RIGHT_SHIELD)/zephyr/zmk.uf2 \
		/firmware/$(RIGHT_UF2)
	@cp "$(FIRMWARE_STAGE)/$(RIGHT_UF2)" \
		"$(FIRMWARE_DIR)/$(RIGHT_UF2)"
	@echo "✓ Right → $(FIRMWARE_DIR)/$(RIGHT_UF2)"
	@echo ""

reset: ## Build settings_reset flasher (RESET_UF2, see keyboard.mk)
	$(call check_init)
	$(call sync_config)
	@echo ""
	@echo "→ Building $(RESET_SHIELD)"
	@mkdir -p "$(FIRMWARE_STAGE)" "$(FIRMWARE_DIR)"
	$(DOCKER_BUILD) west build \
		-s /workspace/zmk/app \
		-d /workspace/build/$(RESET_SHIELD) \
		-b $(BUILD_BOARD) \
		-- \
		-DZMK_CONFIG=/workspace/config \
		-DSHIELD="$(RESET_SHIELD)"
	$(DOCKER_CP) cp /workspace/build/$(RESET_SHIELD)/zephyr/zmk.uf2 \
		/firmware/$(RESET_UF2)
	@cp "$(FIRMWARE_STAGE)/$(RESET_UF2)" \
		"$(FIRMWARE_DIR)/$(RESET_UF2)"
	@echo "✓ Reset → $(FIRMWARE_DIR)/$(RESET_UF2)"
	@echo ""

# ─── Remote status display ────────────────────────────────────────────────────
#
# THIS BRANCH'S ACTUAL PRODUCT. The keyboard targets above are inherited from
# master and are kept only so the file is not surprising; nothing on this branch
# changes them, and build.yaml deliberately does not build them in CI.
#
# A SEPARATE WEST WORKSPACE, AND WHY IT IS NOT OPTIONAL. config/west.yml on this
# branch pins ZMK `main`; the keyboard branches pin v0.2.1. A west workspace
# holds exactly one checkout of ZMK, so running `make init`/`make update` here
# against the shared `zmk-workspace` volume would silently roll the keyboard's
# ZMK to main -- the next `make right` would then build the daily driver against
# an unintended tree. Hence DISPSCAN_VOL and its own sentinel: the two lines
# cannot collide no matter which order they are built in.
#
# First run needs `make dispscan-init` (a fresh ~1-2 GB west update into the new
# volume). After that, `make dispscan`.
DISPSCAN_BOARD  := xiao_ble/nrf52840/zmk
DISPSCAN_SHIELD := dispscan nice_view
DISPSCAN_UF2    := dispscan-xiao_ble-zmk.uf2

# UI-validation image: CONFIG_DISPSCAN_OBSERVER=n, which makes
# CONFIG_DISPSCAN_FAKE_SOURCE default y (they are mutually exclusive in
# Kconfig). Drives the panel from a synthetic status struct, so every widget,
# every value extreme and all three display states are exercised on one flash
# with no keyboard broadcasting -- and the screen carries a "FAKE" marker in
# every state so the device can never be mistaken for one showing real data.
# Distinct filename because both images are flashable and confusing them would
# be easy.
DISPSCAN_FAKE_UF2 := dispscan-FAKE-xiao_ble-zmk.uf2

DISPSCAN_VOL      := zmk-workspace-dispscan
DISPSCAN_STAGE    := $(HOME)/Docker/zmk-config-dispscan
DISPSCAN_SENTINEL := $(HOME)/Docker/.zmk-dispscan-initialized

DISPSCAN_WEST = docker run --rm \
	-v "$(DISPSCAN_VOL):/workspace" \
	-v "$(DISPSCAN_STAGE):/workspace/config" \
	-e HOME=/workspace \
	-w /workspace \
	$(ZMK_IMAGE)

DISPSCAN_BUILD = docker run --rm \
	-v "$(DISPSCAN_VOL):/workspace" \
	-v "$(DISPSCAN_STAGE):/workspace/config" \
	-v "$(FIRMWARE_STAGE):/firmware" \
	-e HOME=/workspace \
	-w /workspace \
	$(ZMK_IMAGE)

DISPSCAN_CP = docker run --rm \
	-v "$(DISPSCAN_VOL):/workspace:ro" \
	-v "$(FIRMWARE_STAGE):/firmware" \
	alpine

define sync_dispscan_config
	@echo "→ Syncing config → $(DISPSCAN_STAGE)"
	@mkdir -p "$(DISPSCAN_STAGE)"
	@rsync -a --delete "$(CONFIG_PATH)/" "$(DISPSCAN_STAGE)/"
endef

dispscan-init: ## Display: first-time west workspace setup (separate volume, ZMK main)
	@echo ""
	@echo "→ Creating Docker volume ($(DISPSCAN_VOL))"
	@docker volume create "$(DISPSCAN_VOL)" > /dev/null
	@docker pull $(ZMK_IMAGE)
	$(call sync_dispscan_config)
	@if [ ! -f "$(DISPSCAN_SENTINEL)" ]; then \
		echo "→ Running west init (manifest: config/west.yml, ZMK main)"; \
		$(DISPSCAN_WEST) west init -l /workspace/config; \
	else \
		echo "→ West already initialized, skipping west init"; \
	fi
	@echo "→ Running west update (~1–2 GB on first run)"
	$(DISPSCAN_WEST) west update
	$(DISPSCAN_WEST) west zephyr-export
	@touch "$(DISPSCAN_SENTINEL)"
	@echo ""
	@echo "✓ Display workspace initialized. Run 'make dispscan'."
	@echo ""

dispscan: ## Display: build the remote status display image (DISPSCAN_UF2)
	@if [ ! -f "$(DISPSCAN_SENTINEL)" ]; then \
		echo ""; echo "✗ Display workspace not initialized. Run: make dispscan-init"; \
		echo ""; exit 1; \
	fi
	$(call sync_dispscan_config)
	@echo ""
	@echo "→ Building $(DISPSCAN_SHIELD) on $(DISPSCAN_BOARD)"
	@mkdir -p "$(FIRMWARE_STAGE)" "$(FIRMWARE_DIR)"
	$(DISPSCAN_BUILD) west build \
		-s /workspace/zmk/app \
		-d /workspace/build/dispscan \
		-b $(DISPSCAN_BOARD) \
		-- \
		-DZMK_CONFIG=/workspace/config \
		-DSHIELD="$(DISPSCAN_SHIELD)"
	$(DISPSCAN_CP) cp /workspace/build/dispscan/zephyr/zmk.uf2 \
		/firmware/$(DISPSCAN_UF2)
	@cp "$(FIRMWARE_STAGE)/$(DISPSCAN_UF2)" "$(FIRMWARE_DIR)/$(DISPSCAN_UF2)"
	@echo "✓ Display → $(FIRMWARE_DIR)/$(DISPSCAN_UF2)"
	@echo ""

dispscan-fake: ## Display: build the UI-validation image (fake data, no radio)
	@if [ ! -f "$(DISPSCAN_SENTINEL)" ]; then \
		echo ""; echo "✗ Display workspace not initialized. Run: make dispscan-init"; \
		echo ""; exit 1; \
	fi
	$(call sync_dispscan_config)
	@echo ""
	@echo "→ Building $(DISPSCAN_SHIELD) with the FAKE source (no BLE observer)"
	@mkdir -p "$(FIRMWARE_STAGE)" "$(FIRMWARE_DIR)"
	$(DISPSCAN_BUILD) west build \
		-s /workspace/zmk/app \
		-d /workspace/build/dispscan-fake \
		-b $(DISPSCAN_BOARD) \
		-- \
		-DZMK_CONFIG=/workspace/config \
		-DSHIELD="$(DISPSCAN_SHIELD)" \
		-DCONFIG_DISPSCAN_OBSERVER=n
	$(DISPSCAN_CP) cp /workspace/build/dispscan-fake/zephyr/zmk.uf2 \
		/firmware/$(DISPSCAN_FAKE_UF2)
	@cp "$(FIRMWARE_STAGE)/$(DISPSCAN_FAKE_UF2)" "$(FIRMWARE_DIR)/$(DISPSCAN_FAKE_UF2)"
	@echo "✓ Display (FAKE) → $(FIRMWARE_DIR)/$(DISPSCAN_FAKE_UF2)"
	@echo ""

# ─── Cache Management ─────────────────────────────────────────────────────────

pristine: ## Delete build caches inside the Docker volume (modules stay intact)
	@echo ""
	@echo "→ Removing build directories from Docker volume"
	@docker run --rm -v "$(WORKSPACE_VOL):/workspace" alpine \
		rm -rf /workspace/build
	@echo "✓ Build caches cleared. Run 'make build' to recompile from scratch."
	@echo ""

clean: ## Full teardown: remove Docker volume, sentinel, and staging dirs
	@echo ""
	@echo "→ Removing Docker volume ($(WORKSPACE_VOL))"
	@docker volume rm "$(WORKSPACE_VOL)" 2>/dev/null || true
	@echo "→ Removing staging dirs and sentinel"
	@rm -rf "$(CONFIG_STAGE)" "$(FIRMWARE_STAGE)"
	@rm -f "$(SENTINEL)"
	@echo "✓ Full clean complete. Run 'make init' to start fresh."
	@echo ""

# ─── Introspection ────────────────────────────────────────────────────────────

firmware: ## List built .uf2 files in the firmware/ output directory
	@echo ""
	@echo "  Firmware output: $(FIRMWARE_DIR)"
	@echo ""
	@ls -lh "$(FIRMWARE_DIR)"/*.uf2 2>/dev/null \
		|| echo "  (no .uf2 files found — run 'make build')"
	@echo ""
