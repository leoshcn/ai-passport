// main/xhs_logic.h
// 与 ESP-IDF / LVGL 无关的仪表盘规则：按键、更新时机、配置是否可用、后端 JSON。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XHS_USERNAME_CAP 64
#define XHS_FETCHED_AT_CAP 32
#define XHS_URL_CAP 192
#define XHS_AVATAR_W 48
#define XHS_AVATAR_H 48
#define XHS_AVATAR_BYTES (XHS_AVATAR_W * XHS_AVATAR_H * 2)
/* 拉取失败后的重试间隔。比正常的每小时/每天更短，避免一次断网就空等一整天。 */
#define XHS_FETCH_RETRY_SEC 60
/* 账号是否换成了另一个创作者。不拉统计，只比对后端给出的修订号。 */
#define XHS_SESSION_POLL_SEC 60
#define XHS_SESSION_REV_CAP 17

typedef enum {
    XHS_PERIOD_HOUR = 0,
    XHS_PERIOD_DAY = 1,
} xhs_period_t;

typedef enum {
    XHS_ITEM_AUTO = 0,
    XHS_ITEM_PERIOD = 1,
    XHS_ITEM_REFRESH = 2,
    XHS_ITEM_REPROVISION = 3,
    XHS_ITEM_COUNT = 4,
} xhs_item_t;

typedef enum {
    XHS_MODE_NAV = 0,
    XHS_MODE_EDIT = 1,
} xhs_mode_t;

typedef enum {
    XHS_VIEW_DASHBOARD = 0,
    XHS_VIEW_SETTINGS = 1,
    XHS_VIEW_FAILURE = 2,
    XHS_VIEW_PROVISION = 3,
    XHS_VIEW_PAIRING = 4,
} xhs_view_t;

typedef enum {
    XHS_PROV_AP = 0,
    XHS_PROV_JOINING = 1,
    XHS_PROV_PAIR = 2,
    XHS_PROV_READY = 3,
} xhs_prov_phase_t;

typedef enum {
    XHS_EVENT_UP_CLICK = 0,
    XHS_EVENT_DOWN_CLICK,
    XHS_EVENT_OK_CLICK,
    XHS_EVENT_OK_LONG,
    XHS_EVENT_FETCH_OK,
    XHS_EVENT_FETCH_FAIL,
} xhs_event_t;

typedef struct {
    bool auto_update;
    xhs_period_t period;
    xhs_item_t selected;
    xhs_mode_t mode;
} xhs_settings_t;

/* request_fetch / save_settings / request_reprovision 只对刚刚处理的那一次事件有效。 */
typedef struct {
    xhs_view_t view;
    xhs_prov_phase_t phase;
    bool storage_ok;
    bool config_ok;
    bool has_stats;
    xhs_settings_t settings;
    bool request_fetch;
    bool save_settings;
    bool request_reprovision;
    /* 已有统计时拉取失败仍留在数据页，用这一位让界面标出失败。成功后清除。 */
    bool update_failed;
    char pair_code[8];
} xhs_app_t;

#define XHS_SSID_CAP 33
#define XHS_PASS_CAP 65
#define XHS_TOKEN_CAP 65
#define XHS_BASE_CAP 128
#define XHS_SETUP_BODY_MAX 480

typedef struct {
    char ssid[XHS_SSID_CAP];
    char password[XHS_PASS_CAP];
    char base_url[XHS_BASE_CAP];
} xhs_setup_form_t;

typedef struct {
    char ssid[XHS_SSID_CAP];
    char password[XHS_PASS_CAP];
    char base_url[XHS_BASE_CAP];
    char token[XHS_TOKEN_CAP];
} xhs_net_cfg_t;

typedef struct {
    bool confirmed;
    char token[XHS_TOKEN_CAP];
    char base_url[XHS_BASE_CAP];
} xhs_pair_reply_t;

typedef struct {
    xhs_prov_phase_t phase;
    xhs_net_cfg_t saved;
    xhs_setup_form_t attempt;
    bool has_attempt;
    bool broadcast;
    char pair_code[8];
    bool stats_kept;
    bool write_wifi;
    bool write_token;
    bool clear_net;
} xhs_prov_t;

typedef struct {
    char username[XHS_USERNAME_CAP];
    char fetched_at[XHS_FETCHED_AT_CAP];
    int64_t fetched_unix;
    int followers;
    int likes_collects;
    int net_followers_7d;
} xhs_stats_t;

/* 存储不可用时停在失败页。空配网记录进入热点，而不是失败页。 */
void xhs_app_init(xhs_app_t *app, bool storage_ok, xhs_prov_phase_t phase,
                  bool has_stats, bool auto_update, xhs_period_t period);

void xhs_app_handle(xhs_app_t *app, xhs_event_t event);

/* 网络任务上报阶段。人还在设置页时不把画面拽走。 */
void xhs_app_note_phase(xhs_app_t *app, xhs_prov_phase_t phase, const char *pair_code);

xhs_view_t xhs_view_for_phase(xhs_prov_phase_t phase);

/* 密码可以为空（开放网络）。令牌至少 8 个可见 ASCII 字符。 */
bool xhs_http_base_ok(const char *base_url);
bool xhs_config_usable(const char *ssid, const char *password,
                       const char *base_url, const char *token);

bool xhs_setup_form_ok(const xhs_setup_form_t *form);
bool xhs_parse_setup_form(const char *body, size_t len, xhs_setup_form_t *out);

bool xhs_pair_code_ok(const char *code);
void xhs_pair_code_from_seed(char out[5], uint32_t seed);
bool xhs_format_pair_hello(const char *code, char *out, size_t out_len);
bool xhs_parse_pair_reply(const char *json, xhs_pair_reply_t *out);

void xhs_prov_init(xhs_prov_t *prov, const xhs_net_cfg_t *saved, uint32_t code_seed);
bool xhs_prov_submit(xhs_prov_t *prov, const xhs_setup_form_t *form);
void xhs_prov_join_failed(xhs_prov_t *prov);
void xhs_prov_join_ok(xhs_prov_t *prov);

/* 认证失败就停止。扫描落空或链路闪断要继续试，直到调用方的截止时间。 */
bool xhs_sta_disconnect_is_final(unsigned reason);
bool xhs_prov_apply_reply(xhs_prov_t *prov, const xhs_pair_reply_t *reply);
void xhs_prov_reconfigure(xhs_prov_t *prov);

bool xhs_join_url(const char *base, const char *path, char *out, size_t out_len);

int64_t xhs_period_seconds(xhs_period_t period);

/*
 * 返回值：0 表示现在就该拉；正数是还要等的秒数；-1 表示自动更新关闭，不要排队。
 * 时钟不可用或还没有成功记录时视为到期，由调用方在成功后再用单调时钟避开紧循环。
 */
int64_t xhs_seconds_until_due(bool auto_update, xhs_period_t period,
                              int64_t now_unix, bool time_valid,
                              int64_t last_unix, bool has_last);

typedef struct {
    char value[XHS_SESSION_REV_CAP];
    bool known;
} xhs_session_seen_t;

/* 读出修订号。空字符串表示当前没有登录。失败时不改 out。 */
bool xhs_parse_session_revision(const char *json, char *out, size_t out_len);

/*
 * 记住修订号。换成另一个非空修订号时返回 true，调用方应立刻拉统计。
 * 第一次见到修订号时，只有 align_saved 为真才返回 true：设备上已有一份统计，
 * 但还不知道它属于哪个账号。退出登录（空修订号）不拉，只把空号记住。
 */
bool xhs_note_session_revision(xhs_session_seen_t *seen, const char *revision,
                               bool align_saved);

/* 只接受本应用后端的扁平 JSON。失败时不改 out。 */
bool xhs_parse_stats(const char *json, xhs_stats_t *out);

void xhs_format_count(char *out, size_t out_len, int value);
void xhs_format_net(char *out, size_t out_len, int value);

/* 按码点截断，避免把 UTF-8 切成半个汉字。dst_len 含结尾 NUL。 */
void xhs_utf8_copy(char *dst, size_t dst_len, const char *src);
bool xhs_utf8_first(const char *src, char *dst, size_t dst_len);
