# =============================================================================
# keyboard.mk — repo-root build knobs (included by the Makefile)
# =============================================================================
#
# Edit these to retarget the local build. Output .uf2 names and the west
# build invocation are derived from them, so there is exactly one place to
# change the board or shield naming.
#
# NOT here on purpose — the Bluetooth/USB device name:
#   The GitHub Actions build only reads files under config/, never the repo
#   root, so the device name lives in  config/charybdis.conf  as
#       CONFIG_ZMK_KEYBOARD_NAME="..."
#   That single line is honoured by both the local build and CI.
# =============================================================================

# ZMK board id (the MCU module). Both halves use the same board.
BOARD        ?= nice_nano_v2

# Shield base name. The split halves are <base>_left / <base>_right.
SHIELD_BASE  ?= charybdis

# Derived shield ids — override individually only if your shields are named
# differently from the <base>_left / <base>_right convention.
LEFT_SHIELD  ?= $(SHIELD_BASE)_left
RIGHT_SHIELD ?= $(SHIELD_BASE)_right
RESET_SHIELD ?= settings_reset

# Derived .uf2 filenames (ZMK convention: <shield>-<board>-zmk.uf2).
LEFT_UF2     := $(LEFT_SHIELD)-$(BOARD)-zmk.uf2
RIGHT_UF2    := $(RIGHT_SHIELD)-$(BOARD)-zmk.uf2
RESET_UF2    := $(RESET_SHIELD)-$(BOARD)-zmk.uf2
