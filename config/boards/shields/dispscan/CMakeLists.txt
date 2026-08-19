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

  # Vendored drawing assets: two 1bpp glyph sets and a pixel font. Data only --
  # no logic was imported and nothing here references ZMK. Provenance, licences
  # and the reason none of the upstream projects could be consumed as a west
  # module instead are in vendor/README.md.
  #
  # Unconditional, unlike the sources below, because the renderer is
  # unconditional: custom_status_screen.c references these symbols whether or
  # not anything is feeding it data, so gating them would break the "with both
  # source symbols n, only the renderer compiles and it still links" property
  # documented further down.
  zephyr_library_sources(
    vendor/dispscan_sym_mods.c
    vendor/dispscan_sym_output.c
    vendor/dispscan_font_pixel_operator.c
  )

  # Development scaffold only -- see the banner in dispscan_fake_source.c. The
  # _ifdef form is what makes CONFIG_DISPSCAN_FAKE_SOURCE=n a genuine removal
  # rather than a runtime no-op: the translation unit is not compiled, so the
  # timer, the work item and their stacks cost nothing in the shipping image.
  # The renderer has no reference to this file, so dropping it still links.
  zephyr_library_sources_ifdef(CONFIG_DISPSCAN_FAKE_SOURCE dispscan_fake_source.c)

  # The real thing: radio -> decoder -> link state -> renderer. Three files
  # because they are three jobs (plan doc, review finding #6) and each is
  # separately reviewable:
  #   dispscan_packet.c   pure bytes-in/struct-out, no kernel objects
  #   dispscan_link.c     the free-running liveness timer, no radio
  #   dispscan_observer.c the scan callback and the D8 binding decision
  #
  # All three are gated on the SAME symbol rather than three, because they are
  # useless apart -- the decoder with nothing feeding it, the link timer with
  # nothing to time. Mutual exclusion with the fake source is enforced in
  # Kconfig (`depends on !DISPSCAN_OBSERVER`), not here, so it holds for anyone
  # reading menuconfig rather than CMake.
  #
  # With both symbols n, only custom_status_screen.c compiles: the renderer
  # links, nothing ever calls dispscan_status_update(), and the panel shows NO
  # SIGNAL -- which for a scanner that is not scanning is the honest answer.
  zephyr_library_sources_ifdef(CONFIG_DISPSCAN_OBSERVER
    dispscan_packet.c
    dispscan_link.c
    dispscan_observer.c
  )

  # Proves the decoder without a radio. See the banner in the file: no
  # broadcaster exists yet, so this is currently the ONLY exercise the six
  # decoder traps get. Nested under the observer by Kconfig, not here.
  zephyr_library_sources_ifdef(CONFIG_DISPSCAN_DECODE_SELFTEST dispscan_decode_test.c)
endif()
