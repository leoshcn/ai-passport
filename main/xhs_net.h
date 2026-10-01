// main/xhs_net.h
// Wi-Fi 和 HTTP 都在独立任务里。结果通过队列交给界面任务，队列项自带头像缓冲。
#pragma once

#include "xhs_logic.h"

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdbool.h>

typedef enum {
    XHS_RESULT_FETCH = 0,
    XHS_RESULT_PHASE = 1,
} xhs_result_kind_t;

typedef struct {
    xhs_result_kind_t kind;
    bool ok;
    bool has_avatar;
    xhs_prov_phase_t phase;
    char pair_code[8];
    xhs_stats_t stats;
    uint8_t avatar[XHS_AVATAR_BYTES];
} xhs_fetch_result_t;

typedef struct {
    bool auto_update;
    xhs_period_t period;
    bool has_last;
    int64_t last_unix;
} xhs_schedule_t;

esp_err_t xhs_net_start(QueueHandle_t results, const xhs_schedule_t *initial,
                        const xhs_net_cfg_t *saved, uint32_t pair_seed);

/* 下面的函数只置标志并唤醒任务，可以在按键处理任务里调用。 */
void xhs_net_request_fetch(void);
void xhs_net_request_reprovision(void);
void xhs_net_set_schedule(bool auto_update, xhs_period_t period, int64_t last_unix, bool has_last);
