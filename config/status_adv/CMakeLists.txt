# SPDX-License-Identifier: MIT
#
# This file is add_subdirectory()'d unconditionally (config/zephyr/module.yml
# points `build.cmake` here). With CONFIG_CHARYBDIS_STATUS_ADV=n it defines no
# library, adds no source, and touches no target — the guard below is the whole
# of D7's "nothing linked in".

if(CONFIG_CHARYBDIS_STATUS_ADV)
  zephyr_library_named(charybdis_status_adv)
  zephyr_library_sources(status_adv.c)

  # ZMK's headers (zmk/ble.h, zmk/keymap.h, zmk/events/*) live in app/include,
  # which app/CMakeLists.txt adds with target_include_directories(app PRIVATE
  # include) — PRIVATE, so it is NOT inherited by modules. ZMK's own
  # app/module/ only exports app/module/include, which is a different set.
  #
  # APPLICATION_SOURCE_DIR is zmk/app for every ZMK build, so this resolves
  # without needing a ZEPHYR_<name>_MODULE_DIR (the zmk repository does not
  # carry a zephyr/module.yml and is therefore not a Zephyr module).
  zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)
endif()
