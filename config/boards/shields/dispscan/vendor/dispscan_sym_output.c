/*
 * VENDORED -- see vendor/README.md before editing.
 *
 * Upstream: englmaxi/zmk-dongle-display, MIT
 *           boards/shields/dongle_display/widgets/output_status_sym.c
 *
 * Endpoint glyphs at LV_COLOR_FORMAT_I1: the USB and Bluetooth marks (9x14)
 * and the three connection states (5x5) -- ok, nok, open.
 *
 * LOCAL CHANGES, both deliberate:
 *
 *   1. Every exported symbol gained a `dispscan_` prefix. These descriptors are
 *      file-scope but not static, and `sym_ok` is exactly the kind of generic
 *      name that collides once something else in the image picks it.
 *
 *   2. sym_1 .. sym_5 (the 5x6 profile digits) were REMOVED. Upstream renders
 *      ZMK's 0-based profile index one-based through that table; our decoded
 *      `profile_slot` is 0-based and the wire can legally carry 0..7, so the
 *      table covers neither end -- there is no sym_0 and nothing above 5. The
 *      renderer draws the digit as text instead, which also keeps the existing
 *      "out of range shows '?' rather than a silent clamp" rule intact.
 *
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

 #include <lvgl.h>


#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif






#ifndef LV_ATTRIBUTE_IMG_SYM_OK
#define LV_ATTRIBUTE_IMG_SYM_OK
#endif

const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST LV_ATTRIBUTE_IMG_SYM_OK uint8_t sym_ok_map[] = {
  0xff, 0xff, 0xff, 0xff, 	/*Color of index 0*/
  0x00, 0x00, 0x00, 0xff, 	/*Color of index 1*/

  0x08, 0x18, 0xb0, 0xe0, 0x40, 
};

const lv_img_dsc_t dispscan_sym_ok = {
  .header.cf = LV_COLOR_FORMAT_I1,
  .header.w = 5,
  .header.h = 5,
  .data_size = 13,
  .data = sym_ok_map,
};

#ifndef LV_ATTRIBUTE_IMG_SYM_NOK
#define LV_ATTRIBUTE_IMG_SYM_NOK
#endif

const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST LV_ATTRIBUTE_IMG_SYM_NOK uint8_t sym_nok_map[] = {
  0xff, 0xff, 0xff, 0xff, 	/*Color of index 0*/
  0x00, 0x00, 0x00, 0xff, 	/*Color of index 1*/

  0x88, 0xd8, 0x70, 0xd8, 0x88, 
};

const lv_img_dsc_t dispscan_sym_nok = {
  .header.cf = LV_COLOR_FORMAT_I1,
  .header.w = 5,
  .header.h = 5,
  .data_size = 13,
  .data = sym_nok_map,
};

#ifndef LV_ATTRIBUTE_IMG_SYM_OPEN
#define LV_ATTRIBUTE_IMG_SYM_OPEN
#endif

const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST LV_ATTRIBUTE_IMG_SYM_OPEN uint8_t sym_open_map[] = {
  0xff, 0xff, 0xff, 0xff, 	/*Color of index 0*/
  0x00, 0x00, 0x00, 0xff, 	/*Color of index 1*/

  0x20, 0x70, 0xd8, 0x70, 0x20, 
};

const lv_img_dsc_t dispscan_sym_open = {
  .header.cf = LV_COLOR_FORMAT_I1,
  .header.w = 5,
  .header.h = 5,
  .data_size = 13,
  .data = sym_open_map,
};

#ifndef LV_ATTRIBUTE_IMG_SYM_BT
#define LV_ATTRIBUTE_IMG_SYM_BT
#endif

const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST LV_ATTRIBUTE_IMG_SYM_BT uint8_t sym_bt_map[] = {
  0xff, 0xff, 0xff, 0xff, 	/*Color of index 0*/
  0x00, 0x00, 0x00, 0xff, 	/*Color of index 1*/

  0x3e, 0x00, 0x67, 0x00, 0xe3, 0x80, 0xe9, 
  0x80, 0x8c, 0x80, 0xc9, 0x80, 0xe3, 0x80,
  0xe3, 0x80, 0xc9, 0x80, 0x8c, 0x80, 0xe9,
  0x80, 0xe3, 0x80, 0x67, 0x00, 0x3e, 0x00, 
};

const lv_img_dsc_t dispscan_sym_bt = {
  .header.cf = LV_COLOR_FORMAT_I1,
  .header.w = 9,
  .header.h = 14,
  .data_size = 36,
  .data = sym_bt_map,
};

#ifndef LV_ATTRIBUTE_IMG_SYM_USB
#define LV_ATTRIBUTE_IMG_SYM_USB
#endif

const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST LV_ATTRIBUTE_IMG_SYM_USB uint8_t sym_usb_map[] = {
  0xff, 0xff, 0xff, 0xff, 	/*Color of index 0*/
  0x00, 0x00, 0x00, 0xff, 	/*Color of index 1*/

  0x7f, 0x00, 0x41, 0x00, 0x55, 0x00, 0x41,
  0x00, 0xff, 0x80, 0x80, 0x80, 0x80, 0x80, 
  0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
  0x80, 0x80, 0x80, 0x80, 0x80, 0xff, 0x80, 
};

const lv_img_dsc_t dispscan_sym_usb = {
  .header.cf = LV_COLOR_FORMAT_I1,
  .header.w = 9,
  .header.h = 14,
  .data_size = 36,
  .data = sym_usb_map,
};



