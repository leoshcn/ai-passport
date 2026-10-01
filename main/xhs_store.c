#include "xhs_store.h"

#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>

static const char *NS = "xhsdash";

esp_err_t xhs_store_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* 不能为了启动仪表盘而擦掉用户已经保存的设置和上次数据。 */
        return err;
    }
    return err;
}

static esp_err_t open_ns(nvs_handle_t *handle)
{
    return nvs_open(NS, NVS_READWRITE, handle);
}

esp_err_t xhs_store_load(xhs_persisted_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->auto_update = true;
    out->period = XHS_PERIOD_DAY;

    nvs_handle_t handle;
    esp_err_t err = open_ns(&handle);
    if (err != ESP_OK) return err;

    uint8_t auto_u = 1;
    if (nvs_get_u8(handle, "auto", &auto_u) == ESP_OK) {
        out->auto_update = auto_u != 0;
    }
    uint8_t period_u = XHS_PERIOD_DAY;
    if (nvs_get_u8(handle, "period", &period_u) == ESP_OK) {
        out->period = period_u == XHS_PERIOD_HOUR ? XHS_PERIOD_HOUR : XHS_PERIOD_DAY;
    }

    uint8_t has = 0;
    if (nvs_get_u8(handle, "has", &has) == ESP_OK && has) {
        int32_t fans = 0;
        int32_t fav = 0;
        int32_t net = 0;
        int64_t unix_s = 0;
        char name[XHS_USERNAME_CAP];
        char when[XHS_FETCHED_AT_CAP];
        size_t name_len = sizeof(name);
        size_t when_len = sizeof(when);
        name[0] = '\0';
        when[0] = '\0';
        esp_err_t stats_err = nvs_get_i32(handle, "fans", &fans);
        if (stats_err == ESP_OK) stats_err = nvs_get_i32(handle, "fav", &fav);
        if (stats_err == ESP_OK) stats_err = nvs_get_i32(handle, "net", &net);
        if (stats_err == ESP_OK) stats_err = nvs_get_i64(handle, "unix", &unix_s);
        if (stats_err == ESP_OK) stats_err = nvs_get_str(handle, "name", name, &name_len);
        if (stats_err == ESP_OK) stats_err = nvs_get_str(handle, "when", when, &when_len);
        if (stats_err == ESP_OK) {
            out->has_stats = true;
            out->stats.followers = fans;
            out->stats.likes_collects = fav;
            out->stats.net_followers_7d = net;
            out->stats.fetched_unix = unix_s;
            xhs_utf8_copy(out->stats.username, sizeof(out->stats.username), name);
            xhs_utf8_copy(out->stats.fetched_at, sizeof(out->stats.fetched_at), when);
            size_t avatar_len = sizeof(out->avatar);
            if (nvs_get_blob(handle, "av", out->avatar, &avatar_len) == ESP_OK &&
                avatar_len == sizeof(out->avatar)) {
                out->has_avatar = true;
            }
        }
    }

    size_t ssid_len = sizeof(out->ssid);
    size_t pass_len = sizeof(out->password);
    size_t url_len = sizeof(out->base_url);
    size_t token_len = sizeof(out->token);
    if (nvs_get_str(handle, "ssid", out->ssid, &ssid_len) == ESP_OK && out->ssid[0] != '\0') {
        out->has_wifi = true;
    } else {
        out->ssid[0] = '\0';
    }
    if (nvs_get_str(handle, "pass", out->password, &pass_len) != ESP_OK) out->password[0] = '\0';
    if (nvs_get_str(handle, "url", out->base_url, &url_len) != ESP_OK) out->base_url[0] = '\0';
    if (nvs_get_str(handle, "tok", out->token, &token_len) != ESP_OK) out->token[0] = '\0';

    nvs_close(handle);
    return ESP_OK;
}

esp_err_t xhs_store_save_settings(bool auto_update, xhs_period_t period)
{
    nvs_handle_t handle;
    esp_err_t err = open_ns(&handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(handle, "auto", auto_update ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, "period", (uint8_t)period);
    }
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t xhs_store_save_stats(const xhs_stats_t *stats,
                               const uint8_t *avatar, bool has_avatar)
{
    if (!stats) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = open_ns(&handle);
    if (err != ESP_OK) return err;

    err = nvs_set_u8(handle, "has", 1);
    if (err == ESP_OK) err = nvs_set_i32(handle, "fans", stats->followers);
    if (err == ESP_OK) err = nvs_set_i32(handle, "fav", stats->likes_collects);
    if (err == ESP_OK) err = nvs_set_i32(handle, "net", stats->net_followers_7d);
    if (err == ESP_OK) err = nvs_set_i64(handle, "unix", stats->fetched_unix);
    if (err == ESP_OK) err = nvs_set_str(handle, "name", stats->username);
    if (err == ESP_OK) err = nvs_set_str(handle, "when", stats->fetched_at);
    if (err == ESP_OK && has_avatar && avatar) {
        err = nvs_set_blob(handle, "av", avatar, XHS_AVATAR_BYTES);
    }
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static esp_err_t put_str(nvs_handle_t handle, const char *key, const char *value)
{
    return nvs_set_str(handle, key, value ? value : "");
}

esp_err_t xhs_store_save_wifi(const char *ssid, const char *password, const char *base_url)
{
    if (!ssid || !password || !base_url) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = open_ns(&handle);
    if (err != ESP_OK) return err;
    err = put_str(handle, "ssid", ssid);
    if (err == ESP_OK) err = put_str(handle, "pass", password);
    if (err == ESP_OK) err = put_str(handle, "url", base_url);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t xhs_store_save_token(const char *token)
{
    if (!token) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = open_ns(&handle);
    if (err != ESP_OK) return err;
    err = put_str(handle, "tok", token);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t xhs_store_clear_provision(void)
{
    nvs_handle_t handle;
    esp_err_t err = open_ns(&handle);
    if (err != ESP_OK) return err;
    const char *keys[] = {"ssid", "pass", "url", "tok"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        esp_err_t erased = nvs_erase_key(handle, keys[i]);
        if (erased != ESP_OK && erased != ESP_ERR_NVS_NOT_FOUND) err = erased;
    }
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}
