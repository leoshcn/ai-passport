// main/xhs_ui.h
// 仪表盘、设置页和失败页。调用方必须已经持有 bsp_lvgl_lock()。
#pragma once

#include "xhs_logic.h"

#include <stdbool.h>
#include <stdint.h>

void xhs_ui_init(void);

void xhs_ui_set_battery(int soc_percent);

void xhs_ui_show(xhs_view_t view, const xhs_settings_t *settings,
                 const xhs_stats_t *stats, bool has_stats,
                 const uint8_t *avatar, bool has_avatar,
                 xhs_prov_phase_t phase, const char *pair_code,
                 bool update_failed);
