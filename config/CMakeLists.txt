# Build-fingerprint module. Compiles build_stamp.c into every image so the
# firmware logs, at boot, exactly which config commit + build time it came from.
# Ties a serial-log line back to a single build (see build_stamp.c / the
# BUILDSTAMP line emitted by scripts/zmk_serial_debug.sh).

zephyr_library()
zephyr_library_sources(build_stamp.c)

# git describe of THIS repo, best-effort. CMAKE_CURRENT_LIST_DIR is the config/
# dir; git walks up to the repo-root .git. In CI the checkout is shallow with no
# tags, so --always yields the short SHA (--tags is a bonus when tags exist).
# ERROR_QUIET + fallback keeps the build working if git/.git is unavailable.
execute_process(
  COMMAND git -C ${CMAKE_CURRENT_LIST_DIR} describe --always --dirty --tags
  OUTPUT_VARIABLE CHARYBDIS_GIT_DESC
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
if(NOT CHARYBDIS_GIT_DESC)
  set(CHARYBDIS_GIT_DESC "nogit")
endif()
message(STATUS "charybdis build_stamp: git=${CHARYBDIS_GIT_DESC}")
zephyr_library_compile_definitions(CHARYBDIS_GIT_DESC="${CHARYBDIS_GIT_DESC}")
