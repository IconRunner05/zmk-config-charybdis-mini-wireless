# Build-fingerprint module. Compiles build_stamp.c into every image so the
# firmware logs, at boot, exactly which config commit + build time it came from.
# Ties a serial-log line back to a single build (see build_stamp.c / the
# BUILDSTAMP line emitted by scripts/zmk_serial_debug.sh).

zephyr_library()
zephyr_library_sources(build_stamp.c)

# git describe of THIS repo, best-effort. CMAKE_CURRENT_LIST_DIR is the config/
# dir; when built locally git walks up to the repo-root .git. --always yields a
# short SHA even on a shallow checkout (--tags is a bonus when tags exist).
execute_process(
  COMMAND git -C ${CMAKE_CURRENT_LIST_DIR} describe --always --dirty --tags
  OUTPUT_VARIABLE CHARYBDIS_GIT_DESC
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
# CI fallback: ZMK stages config/ into the west workspace with no parent .git,
# so git describe is empty there. GitHub Actions always exports GITHUB_SHA for
# the built commit -- use its short form so CI images stay commit-traceable.
if(NOT CHARYBDIS_GIT_DESC AND DEFINED ENV{GITHUB_SHA})
  string(SUBSTRING "$ENV{GITHUB_SHA}" 0 8 CHARYBDIS_GIT_DESC)
  set(CHARYBDIS_GIT_DESC "ci-${CHARYBDIS_GIT_DESC}")
endif()
if(NOT CHARYBDIS_GIT_DESC)
  set(CHARYBDIS_GIT_DESC "nogit")
endif()
message(STATUS "charybdis build_stamp: git=${CHARYBDIS_GIT_DESC}")
zephyr_library_compile_definitions(CHARYBDIS_GIT_DESC="${CHARYBDIS_GIT_DESC}")
