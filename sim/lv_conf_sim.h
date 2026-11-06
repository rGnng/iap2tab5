/**
 * @file lv_conf_sim.h
 * Simulator-only LVGL configuration.
 *
 * The Tab5 target renders in RGB565 (see ../lv_conf.h).  In the desktop
 * simulator that format quantizes neutral UI grays into greenish values,
 * e.g. #141414 becomes #101410 on screen, while the PNG screenshots are
 * rendered in ARGB8888 and stay neutral.  Use 32-bit color here so the
 * simulator matches its screenshots.
 */

#ifndef IAP2TAB5_LV_CONF_SIM_H
#define IAP2TAB5_LV_CONF_SIM_H

#include "../lv_conf.h"

#undef LV_COLOR_FORMAT_DEFAULT
#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_ARGB8888

#endif /*IAP2TAB5_LV_CONF_SIM_H*/
