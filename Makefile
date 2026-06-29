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

.PHONY: help init update build left right reset pristine clean firmware

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
