// main/main.c —— 小红书创作者数据仪表盘。
//
// 按键（上 / 下 / 确定）：
//   仪表盘：确定短按熄屏。熄屏后再按上、下或确定只点亮，长按确定也只点亮。
//   点亮后长按确定进入设置。
//   设置：上/下切换项目，确定进入调整；调整中上/下改值，确定写回。
//   「立即刷新」按一次确定回到仪表盘；已配网时立刻拉取，不进入调整。
//   「重新配网」再按确定后清除 Wi-Fi、后端地址和令牌，并回到热点。统计数据保留。
//   设置里长按确定回到当前主画面。
// 没有 NVS 配网记录时打开热点。存储初始化失败才停在失败页，并且不擦除分区。
#include "xhs_logic.h"
#include "xhs_net.h"
#include "xhs_store.h"
#include "xhs_ui.h"

#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_pins.h"

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "xhs";

#define BUTTON_QUEUE_DEPTH 8
#define RESULT_QUEUE_DEPTH 2
#define BL_ACTIVE 70
#define BL_IDLE 8
#define IDLE_MS 30000

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} button_event_t;

static xhs_app_t s_app;
static xhs_persisted_t s_saved;
static QueueHandle_t s_buttons;
static QueueHandle_t s_results;
static TaskHandle_t s_input_task;
static volatile bool s_input_ready;
static TickType_t s_active_tick;
static bool s_dimmed;
static bool s_net_up;
static xhs_fetch_result_t s_fetch_result;
static int s_battery_ticks;

static void present(void)
{
    int soc = bsp_battery_soc();
    if (!bsp_lvgl_lock(500)) return;
    xhs_ui_show(s_app.view, &s_app.settings, &s_saved.stats, s_saved.has_stats,
                s_saved.avatar, s_saved.has_avatar, s_app.phase, s_app.pair_code,
                s_app.update_failed);
    xhs_ui_set_battery(soc);
    bsp_lvgl_unlock();
}

static void push_schedule(void)
{
    if (!s_net_up) return;
    xhs_net_set_schedule(s_app.settings.auto_update, s_app.settings.period,
                         s_saved.stats.fetched_unix, s_saved.has_stats);
}

static void apply_backlight(bool was_dimmed, bool was_standby)
{
    if (s_app.standby) {
        bsp_display_backlight(0);
        s_dimmed = true;
        return;
    }
    if (was_standby || was_dimmed) {
        bsp_display_backlight(BL_ACTIVE);
        s_dimmed = false;
    }
}

static void handle_button(bsp_btn_t btn, bsp_btn_ev_t event)
{
    /* 按下和双击不映射。button 4.2.0 在双击成立时不再另报单击。 */
    xhs_event_t mapped;
    if (event == BSP_BTN_LONG && btn == BSP_BTN_OK) mapped = XHS_EVENT_OK_LONG;
    else if (event == BSP_BTN_CLICK && btn == BSP_BTN_UP) mapped = XHS_EVENT_UP_CLICK;
    else if (event == BSP_BTN_CLICK && btn == BSP_BTN_DOWN) mapped = XHS_EVENT_DOWN_CLICK;
    else if (event == BSP_BTN_CLICK && btn == BSP_BTN_OK) mapped = XHS_EVENT_OK_CLICK;
    else return;

    bool was_dimmed = s_dimmed;
    bool was_standby = s_app.standby;
    s_active_tick = xTaskGetTickCount();
    xhs_app_handle(&s_app, mapped);
    if (s_app.save_settings) {
        if (xhs_store_save_settings(s_app.settings.auto_update, s_app.settings.period) != ESP_OK) {
            ESP_LOGW(TAG, "settings were not saved");
        }
        push_schedule();
    }
    if (s_app.request_fetch && s_net_up) xhs_net_request_fetch();
    if (s_app.request_reprovision && s_net_up) xhs_net_request_reprovision();
    present();
    apply_backlight(was_dimmed, was_standby);
}

static void handle_result(const xhs_fetch_result_t *result)
{
    if (result->kind == XHS_RESULT_PHASE) {
        xhs_app_note_phase(&s_app, result->phase, result->pair_code);
        if (s_app.config_ok) push_schedule();
        present();
        return;
    }
    if (result->ok) {
        s_saved.stats = result->stats;
        s_saved.has_stats = true;
        if (result->has_avatar) {
            memcpy(s_saved.avatar, result->avatar, sizeof(s_saved.avatar));
            s_saved.has_avatar = true;
        }
        if (xhs_store_save_stats(&s_saved.stats,
                                 s_saved.has_avatar ? s_saved.avatar : NULL,
                                 s_saved.has_avatar) != ESP_OK) {
            ESP_LOGW(TAG, "stats were not saved");
        }
        xhs_view_t before = s_app.view;
        xhs_app_handle(&s_app, XHS_EVENT_FETCH_OK);
        /* 成功时间由网络任务自己记下。这里再 set_schedule 会清掉等待并在时钟未同步时立刻重拉。 */
        if (before == XHS_VIEW_SETTINGS && s_app.view == XHS_VIEW_SETTINGS) return;
    } else {
        xhs_view_t before = s_app.view;
        xhs_app_handle(&s_app, XHS_EVENT_FETCH_FAIL);
        if (s_app.view == before && !s_app.update_failed) return;
        if (s_app.view == XHS_VIEW_SETTINGS) return;
    }
    present();
}

static void input_task(void *arg)
{
    (void)arg;
    s_active_tick = xTaskGetTickCount();
    for (;;) {
        button_event_t button;
        if (s_buttons && xQueueReceive(s_buttons, &button, pdMS_TO_TICKS(500)) == pdTRUE) {
            handle_button(button.btn, button.event);
        } else if (!s_app.standby && !s_dimmed &&
                   (xTaskGetTickCount() - s_active_tick) >= pdMS_TO_TICKS(IDLE_MS)) {
            bsp_display_backlight(BL_IDLE);
            s_dimmed = true;
        }

        if (s_results && xQueueReceive(s_results, &s_fetch_result, 0) == pdTRUE) {
            handle_result(&s_fetch_result);
        } else if (!s_dimmed && ++s_battery_ticks >= 10) {
            s_battery_ticks = 0;
            int soc = bsp_battery_soc();
            if (bsp_lvgl_lock(100)) {
                xhs_ui_set_battery(soc);
                bsp_lvgl_unlock();
            }
        }
    }
}

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (!s_input_ready || !s_buttons) return;
    const button_event_t event = { .btn = btn, .event = ev };
    (void)xQueueSend(s_buttons, &event, 0);
}

static void copy_field(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    dst[0] = '\0';
    if (!src) return;
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void app_main(void)
{
    ESP_LOGI(TAG, "xiaohongshu dashboard");
    memset(&s_saved, 0, sizeof(s_saved));
    s_saved.auto_update = true;
    s_saved.period = XHS_PERIOD_DAY;
    bool storage_ok = xhs_store_init() == ESP_OK && xhs_store_load(&s_saved) == ESP_OK;
    if (!storage_ok) {
        ESP_LOGE(TAG, "nvs unavailable");
        memset(&s_saved, 0, sizeof(s_saved));
        s_saved.auto_update = true;
        s_saved.period = XHS_PERIOD_DAY;
    }

    xhs_net_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    uint32_t seed = 0;
    xhs_prov_phase_t phase = XHS_PROV_AP;
    char pair_code[8];
    memset(pair_code, 0, sizeof(pair_code));
    if (storage_ok) {
        copy_field(cfg.ssid, sizeof(cfg.ssid), s_saved.ssid);
        copy_field(cfg.password, sizeof(cfg.password), s_saved.password);
        copy_field(cfg.base_url, sizeof(cfg.base_url), s_saved.base_url);
        copy_field(cfg.token, sizeof(cfg.token), s_saved.token);
        seed = esp_random();
        xhs_prov_t boot;
        xhs_prov_init(&boot, &cfg, seed);
        phase = boot.phase;
        memcpy(pair_code, boot.pair_code, sizeof(pair_code));
        memset(&boot, 0, sizeof(boot));
    }

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        memset(&cfg, 0, sizeof(cfg));
        ESP_LOGE(TAG, "display init failed (MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(BL_ACTIVE);
    (void)bsp_battery_init();

    xhs_app_init(&s_app, storage_ok, phase, s_saved.has_stats, s_saved.auto_update, s_saved.period);
    if (pair_code[0]) memcpy(s_app.pair_code, pair_code, sizeof(s_app.pair_code));
    if (bsp_lvgl_lock(1000)) {
        xhs_ui_init();
        xhs_ui_show(s_app.view, &s_app.settings, &s_saved.stats, s_saved.has_stats,
                    s_saved.avatar, s_saved.has_avatar, s_app.phase, s_app.pair_code,
                    s_app.update_failed);
        xhs_ui_set_battery(bsp_battery_soc());
        bsp_lvgl_unlock();
    }

    s_buttons = xQueueCreate(BUTTON_QUEUE_DEPTH, sizeof(button_event_t));
    if (storage_ok) {
        s_results = xQueueCreate(RESULT_QUEUE_DEPTH, sizeof(xhs_fetch_result_t));
        xhs_schedule_t schedule = {
            .auto_update = s_saved.auto_update,
            .period = s_saved.period,
            .has_last = s_saved.has_stats,
            .last_unix = s_saved.stats.fetched_unix,
        };
        if (s_results && xhs_net_start(s_results, &schedule, &cfg, seed) == ESP_OK) s_net_up = true;
        else ESP_LOGE(TAG, "network task was not started");
    }
    memset(&cfg, 0, sizeof(cfg));

    if (s_buttons && xTaskCreate(input_task, "xhs_ui", 8192, NULL, 5, &s_input_task) == pdPASS) {
        if (bsp_button_init(on_key, NULL) == ESP_OK) s_input_ready = true;
        else ESP_LOGE(TAG, "buttons unavailable");
    } else {
        ESP_LOGE(TAG, "input task was not started");
    }
}
