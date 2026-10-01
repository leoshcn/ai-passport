// main/xhs_store.h
// 设置和上一次成功拉到的数据放在 NVS。初始化失败不擦除分区。
#pragma once

#include "xhs_logic.h"

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool auto_update;
    xhs_period_t period;
    bool has_stats;
    xhs_stats_t stats;
    bool has_avatar;
    uint8_t avatar[XHS_AVATAR_BYTES];
    bool has_wifi;
    char ssid[XHS_SSID_CAP];
    char password[XHS_PASS_CAP];
    char base_url[XHS_BASE_CAP];
    char token[XHS_TOKEN_CAP];
} xhs_persisted_t;

/* NVS 不可用时返回错误。调用方应停在失败页，而不是擦掉已有记录。 */
esp_err_t xhs_store_init(void);

/* 缺键时用默认值：自动更新开、每天一次、没有历史数据。 */
esp_err_t xhs_store_load(xhs_persisted_t *out);

esp_err_t xhs_store_save_settings(bool auto_update, xhs_period_t period);

/* avatar 为空或 has_avatar 为假时保留已经存过的头像。 */
esp_err_t xhs_store_save_stats(const xhs_stats_t *stats,
                               const uint8_t *avatar, bool has_avatar);

/* 只在站点拿到 IP 之后调用。失败的尝试不要写进来。 */
esp_err_t xhs_store_save_wifi(const char *ssid, const char *password, const char *base_url);

esp_err_t xhs_store_save_token(const char *token);

/* 清除 Wi-Fi、后端地址和令牌。统计数据的键保留。 */
esp_err_t xhs_store_clear_provision(void);
