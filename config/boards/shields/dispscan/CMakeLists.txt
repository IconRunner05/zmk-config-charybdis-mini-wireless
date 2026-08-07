# dispscan — shield sources.
#
# Zephyr's boards/shields/CMakeLists.txt add_subdirectory()s every shield dir
# that has a CMakeLists.txt, including out-of-tree ones under
# <zmk-config>/boards/shields, so this file is picked up automatically.
#
# Unlike ZMK's in-tree nice_view shield we create the library explicitly:
# zephyr_library_sources() writes into ${ZEPHYR_CURRENT_LIBRARY}, which is only
# guaranteed to be set after zephyr_library().
if(CONFIG_ZMK_DISPLAY)
  zephyr_library()
  # ZMK's app does `target_include_directories(app PRIVATE include)`, i.e. its
  # headers are NOT public. Shield libraries have to add app/include themselves
  # to reach <zmk/display/status_screen.h>; CMAKE_SOURCE_DIR is zmk/app here.
  # Same line as app/boards/shields/nice_view/CMakeLists.txt.
  zephyr_library_include_directories(${CMAKE_SOURCE_DIR}/include)
  zephyr_library_sources(custom_status_screen.c)

  # Development scaffold only -- see the banner in dispscan_fake_source.c. The
  # _ifdef form is what makes CONFIG_DISPSCAN_FAKE_SOURCE=n a genuine removal
  # rather than a runtime no-op: the translation unit is not compiled, so the
  # timer, the work item and their stacks cost nothing in the shipping image.
  # The renderer has no reference to this file, so dropping it still links.
  zephyr_library_sources_ifdef(CONFIG_DISPSCAN_FAKE_SOURCE dispscan_fake_source.c)
endif()
