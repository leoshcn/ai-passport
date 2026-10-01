#include "xhs_logic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int utf8_span(const unsigned char *s, size_t remain)
{
    if (remain == 0 || s[0] == 0) return 0;
    if (s[0] < 0x80) return 1;
    int need = 0;
    if ((s[0] & 0xE0) == 0xC0) need = 2;
    else if ((s[0] & 0xF0) == 0xE0) need = 3;
    else if ((s[0] & 0xF8) == 0xF0) need = 4;
    else return 0;
    if ((size_t)need > remain) return 0;
    for (int i = 1; i < need; i++) {
        if ((s[i] & 0xC0) != 0x80) return 0;
    }
    return need;
}

void xhs_utf8_copy(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
    if (!src) return;

    size_t used = 0;
    const unsigned char *p = (const unsigned char *)src;
    while (*p) {
        int span = utf8_span(p, strlen((const char *)p));
        if (span <= 0) break;
        if (used + (size_t)span >= dst_len) break;
        memcpy(dst + used, p, (size_t)span);
        used += (size_t)span;
        p += span;
    }
    dst[used] = '\0';
}

bool xhs_utf8_first(const char *src, char *dst, size_t dst_len)
{
    if (!dst || dst_len == 0) return false;
    dst[0] = '\0';
    if (!src || !src[0]) return false;
    int span = utf8_span((const unsigned char *)src, strlen(src));
    if (span <= 0 || (size_t)span >= dst_len) return false;
    memcpy(dst, src, (size_t)span);
    dst[span] = '\0';
    return true;
}

static bool ascii_token(const char *token)
{
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)token; *p; p++) {
        if (*p <= 0x20 || *p >= 0x7F) return false;
        n++;
        if (n > 64) return false;
    }
    return n >= 8;
}

bool xhs_http_base_ok(const char *base_url)
{
    if (!base_url) return false;
    if (strncmp(base_url, "http://", 7) != 0) return false;
    size_t url_len = strlen(base_url);
    if (url_len < 12 || url_len > 120) return false;
    if (strchr(base_url, ' ') != NULL) return false;
    if (base_url[7] == '\0' || base_url[7] == '/') return false;
    return true;
}

bool xhs_config_usable(const char *ssid, const char *password,
                       const char *base_url, const char *token)
{
    if (!ssid || !password || !base_url || !token) return false;
    size_t ssid_len = strlen(ssid);
    if (ssid_len == 0 || ssid_len > 32) return false;
    if (strlen(password) > 64) return false;
    return xhs_http_base_ok(base_url) && ascii_token(token);
}

bool xhs_join_url(const char *base, const char *path, char *out, size_t out_len)
{
    if (!base || !path || !out || out_len == 0 || path[0] != '/') return false;
    size_t n = strlen(base);
    while (n > 0 && base[n - 1] == '/') n--;
    size_t path_len = strlen(path);
    if (n + path_len + 1 > out_len) return false;
    memcpy(out, base, n);
    memcpy(out + n, path, path_len + 1);
    return true;
}

int64_t xhs_period_seconds(xhs_period_t period)
{
    return period == XHS_PERIOD_HOUR ? 3600 : 86400;
}

int64_t xhs_seconds_until_due(bool auto_update, xhs_period_t period,
                              int64_t now_unix, bool time_valid,
                              int64_t last_unix, bool has_last)
{
    if (!auto_update) return -1;
    if (!has_last || last_unix <= 0 || !time_valid) return 0;
    int64_t age = now_unix - last_unix;
    int64_t period_s = xhs_period_seconds(period);
    if (age < 0 || age >= period_s) return 0;
    return period_s - age;
}

xhs_view_t xhs_view_for_phase(xhs_prov_phase_t phase)
{
    if (phase == XHS_PROV_READY) return XHS_VIEW_DASHBOARD;
    if (phase == XHS_PROV_PAIR) return XHS_VIEW_PAIRING;
    return XHS_VIEW_PROVISION;
}

void xhs_app_init(xhs_app_t *app, bool storage_ok, xhs_prov_phase_t phase,
                  bool has_stats, bool auto_update, xhs_period_t period)
{
    if (!app) return;
    memset(app, 0, sizeof(*app));
    app->storage_ok = storage_ok;
    app->has_stats = has_stats;
    app->phase = phase;
    app->config_ok = storage_ok && phase == XHS_PROV_READY;
    app->view = storage_ok ? xhs_view_for_phase(phase) : XHS_VIEW_FAILURE;
    app->settings.auto_update = auto_update;
    app->settings.period = period == XHS_PERIOD_HOUR ? XHS_PERIOD_HOUR : XHS_PERIOD_DAY;
    app->settings.selected = XHS_ITEM_AUTO;
    app->settings.mode = XHS_MODE_NAV;
}

void xhs_app_note_phase(xhs_app_t *app, xhs_prov_phase_t phase, const char *pair_code)
{
    if (!app || !app->storage_ok) return;
    app->phase = phase;
    app->config_ok = phase == XHS_PROV_READY;
    if (pair_code && pair_code[0]) {
        xhs_utf8_copy(app->pair_code, sizeof(app->pair_code), pair_code);
    } else if (phase != XHS_PROV_PAIR) {
        app->pair_code[0] = '\0';
    }
    if (app->view == XHS_VIEW_SETTINGS) return;
    app->view = xhs_view_for_phase(phase);
}

static void adjust_selected(xhs_settings_t *settings)
{
    if (settings->selected == XHS_ITEM_AUTO) {
        settings->auto_update = !settings->auto_update;
        return;
    }
    settings->period = settings->period == XHS_PERIOD_HOUR
        ? XHS_PERIOD_DAY : XHS_PERIOD_HOUR;
}

static void move_selection(xhs_settings_t *settings, int delta)
{
    int count = (int)XHS_ITEM_COUNT;
    int next = ((int)settings->selected + delta) % count;
    if (next < 0) next += count;
    settings->selected = (xhs_item_t)next;
}

void xhs_app_handle(xhs_app_t *app, xhs_event_t event)
{
    if (!app) return;
    app->request_fetch = false;
    app->save_settings = false;
    app->request_reprovision = false;

    if (event == XHS_EVENT_FETCH_OK) {
        app->has_stats = true;
        if (app->view == XHS_VIEW_FAILURE && app->config_ok) {
            app->view = XHS_VIEW_DASHBOARD;
        }
        return;
    }
    if (event == XHS_EVENT_FETCH_FAIL) {
        if (app->phase == XHS_PROV_READY && !app->has_stats && app->view != XHS_VIEW_SETTINGS) {
            app->view = XHS_VIEW_FAILURE;
        }
        return;
    }

    if (!app->storage_ok) return;

    if (event == XHS_EVENT_OK_LONG) {
        if (app->view == XHS_VIEW_SETTINGS) {
            app->view = xhs_view_for_phase(app->phase);
            app->settings.mode = XHS_MODE_NAV;
            app->save_settings = true;
        } else {
            app->view = XHS_VIEW_SETTINGS;
            app->settings.mode = XHS_MODE_NAV;
            app->settings.selected = XHS_ITEM_AUTO;
        }
        return;
    }

    if (app->view == XHS_VIEW_DASHBOARD) {
        if (event == XHS_EVENT_OK_CLICK && app->config_ok) app->request_fetch = true;
        return;
    }

    if (app->view != XHS_VIEW_SETTINGS) return;

    if (app->settings.mode == XHS_MODE_NAV) {
        if (event == XHS_EVENT_UP_CLICK) move_selection(&app->settings, -1);
        else if (event == XHS_EVENT_DOWN_CLICK) move_selection(&app->settings, 1);
        else if (event == XHS_EVENT_OK_CLICK) app->settings.mode = XHS_MODE_EDIT;
        return;
    }

    if (app->settings.selected == XHS_ITEM_REPROVISION) {
        if (event == XHS_EVENT_OK_CLICK) {
            app->request_reprovision = true;
            app->settings.mode = XHS_MODE_NAV;
            app->phase = XHS_PROV_AP;
            app->config_ok = false;
            app->view = XHS_VIEW_PROVISION;
            app->pair_code[0] = '\0';
        }
        return;
    }

    if (event == XHS_EVENT_UP_CLICK || event == XHS_EVENT_DOWN_CLICK) {
        adjust_selected(&app->settings);
    } else if (event == XHS_EVENT_OK_CLICK) {
        app->settings.mode = XHS_MODE_NAV;
        app->save_settings = true;
    }
}

static void format_grouped(char *out, size_t out_len, int value, bool force_sign)
{
    if (!out || out_len == 0) return;
    out[0] = '\0';

    unsigned int mag;
    char sign = 0;
    if (value < 0) {
        sign = '-';
        mag = (unsigned int)(-(value + 1)) + 1U;
    } else {
        mag = (unsigned int)value;
        if (force_sign && value > 0) sign = '+';
    }

    char digits[16];
    int n = snprintf(digits, sizeof(digits), "%u", mag);
    if (n <= 0 || (size_t)n >= sizeof(digits)) return;

    size_t commas = n > 3 ? (size_t)((n - 1) / 3) : 0;
    size_t total = (size_t)n + commas + (sign ? 1U : 0U);
    if (total + 1 > out_len) {
        snprintf(out, out_len, "%d", value);
        return;
    }

    char *w = out;
    if (sign) *w++ = sign;
    int first = n % 3;
    if (first == 0) first = 3;
    memcpy(w, digits, (size_t)first);
    w += first;
    for (int i = first; i < n; i += 3) {
        *w++ = ',';
        memcpy(w, digits + i, 3);
        w += 3;
    }
    *w = '\0';
}

void xhs_format_count(char *out, size_t out_len, int value)
{
    format_grouped(out, out_len, value, false);
}

void xhs_format_net(char *out, size_t out_len, int value)
{
    format_grouped(out, out_len, value, true);
}

static const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    return s;
}

static const char *find_value(const char *json, const char *key)
{
    char pattern[48];
    if (snprintf(pattern, sizeof(pattern), "\"%s\"", key) >= (int)sizeof(pattern)) {
        return NULL;
    }
    const char *p = json;
    size_t pat_len = strlen(pattern);
    while ((p = strstr(p, pattern)) != NULL) {
        const char *q = skip_ws(p + pat_len);
        if (*q == ':') return skip_ws(q + 1);
        p += pat_len;
    }
    return NULL;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool decode_string(const char *value, char *dst, size_t dst_len)
{
    if (*value != '"') return false;
    char tmp[XHS_USERNAME_CAP > XHS_FETCHED_AT_CAP ? XHS_USERNAME_CAP : XHS_FETCHED_AT_CAP];
    if (dst_len > sizeof(tmp)) return false;
    size_t used = 0;
    const char *p = value + 1;
    while (*p && *p != '"') {
        unsigned char produced[4];
        int count = 0;
        if (*p == '\\') {
            p++;
            if (*p == 'u') {
                int cp = 0;
                for (int i = 1; i <= 4; i++) {
                    int nib = hex_nibble(p[i]);
                    if (nib < 0) return false;
                    cp = (cp << 4) | nib;
                }
                p += 5;
                if (cp < 0x80) {
                    produced[0] = (unsigned char)cp;
                    count = 1;
                } else if (cp < 0x800) {
                    produced[0] = (unsigned char)(0xC0 | (cp >> 6));
                    produced[1] = (unsigned char)(0x80 | (cp & 0x3F));
                    count = 2;
                } else {
                    produced[0] = (unsigned char)(0xE0 | (cp >> 12));
                    produced[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
                    produced[2] = (unsigned char)(0x80 | (cp & 0x3F));
                    count = 3;
                }
            } else {
                char ch = *p++;
                if (ch == '"' || ch == '\\' || ch == '/') produced[0] = (unsigned char)ch;
                else if (ch == 'n') produced[0] = '\n';
                else if (ch == 'r') produced[0] = '\r';
                else if (ch == 't') produced[0] = '\t';
                else return false;
                count = 1;
            }
        } else {
            int span = utf8_span((const unsigned char *)p, strlen(p));
            if (span <= 0) return false;
            if (span > 4) return false;
            memcpy(produced, p, (size_t)span);
            count = span;
            p += span;
        }
        /* 超长昵称截断，而不是让整次解析失败。后面的字节继续扫完，避免引号对不齐。 */
        if (used + (size_t)count < sizeof(tmp)) {
            memcpy(tmp + used, produced, (size_t)count);
            used += (size_t)count;
        }
    }
    if (*p != '"') return false;
    tmp[used] = '\0';
    xhs_utf8_copy(dst, dst_len, tmp);
    return true;
}

static bool parse_i64(const char *value, int64_t *out)
{
    if (*value != '-' && (*value < '0' || *value > '9')) return false;
    char *end = NULL;
    long long parsed = strtoll(value, &end, 10);
    if (end == value) return false;
    *out = (int64_t)parsed;
    return true;
}

bool xhs_parse_stats(const char *json, xhs_stats_t *out)
{
    if (!json || !out) return false;
    const char *ok = find_value(json, "ok");
    if (!ok || strncmp(ok, "true", 4) != 0) return false;

    xhs_stats_t parsed;
    memset(&parsed, 0, sizeof(parsed));

    const char *username = find_value(json, "username");
    const char *fetched = find_value(json, "fetched_at");
    const char *followers = find_value(json, "followers");
    const char *likes = find_value(json, "likes_collects");
    const char *net = find_value(json, "net_followers_7d");
    const char *unix_s = find_value(json, "fetched_unix");
    if (!username || !fetched || !followers || !likes || !net || !unix_s) return false;
    if (!decode_string(username, parsed.username, sizeof(parsed.username))) return false;
    if (!decode_string(fetched, parsed.fetched_at, sizeof(parsed.fetched_at))) return false;

    int64_t followers_v = 0;
    int64_t likes_v = 0;
    int64_t net_v = 0;
    int64_t unix_v = 0;
    if (!parse_i64(followers, &followers_v) || !parse_i64(likes, &likes_v) ||
        !parse_i64(net, &net_v) || !parse_i64(unix_s, &unix_v)) {
        return false;
    }
    if (followers_v > 2147483647LL || likes_v > 2147483647LL ||
        followers_v < 0 || likes_v < 0 ||
        net_v > 2147483647LL || net_v < -2147483647LL) {
        return false;
    }
    parsed.followers = (int)followers_v;
    parsed.likes_collects = (int)likes_v;
    parsed.net_followers_7d = (int)net_v;
    parsed.fetched_unix = unix_v;
    *out = parsed;
    return true;
}

static const char XHS_PAIR_ALPHABET[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";

bool xhs_pair_code_ok(const char *code)
{
    if (!code || strlen(code) != 4) return false;
    for (int i = 0; i < 4; i++) {
        if (strchr(XHS_PAIR_ALPHABET, code[i]) == NULL) return false;
    }
    return true;
}

void xhs_pair_code_from_seed(char out[5], uint32_t seed)
{
    if (!out) return;
    size_t n = strlen(XHS_PAIR_ALPHABET);
    for (int i = 0; i < 4; i++) {
        out[i] = XHS_PAIR_ALPHABET[seed % (uint32_t)n];
        seed /= (uint32_t)n;
    }
    out[4] = '\0';
}

bool xhs_format_pair_hello(const char *code, char *out, size_t out_len)
{
    if (!out || out_len < 16 || !xhs_pair_code_ok(code)) return false;
    int n = snprintf(out, out_len, "{\"code\":\"%s\"}", code);
    return n > 0 && (size_t)n < out_len;
}

static bool url_decode(const char *in, size_t in_len, char *out, size_t out_cap)
{
    if (!out || out_cap == 0) return false;
    size_t used = 0;
    for (size_t i = 0; i < in_len; i++) {
        unsigned char ch = (unsigned char)in[i];
        if (ch == '+') {
            ch = ' ';
        } else if (ch == '%') {
            if (i + 2 >= in_len) return false;
            int hi = hex_nibble(in[i + 1]);
            int lo = hex_nibble(in[i + 2]);
            if (hi < 0 || lo < 0) return false;
            ch = (unsigned char)((hi << 4) | lo);
            if (ch == 0) return false;
            i += 2;
        }
        if (used + 1 >= out_cap) return false;
        out[used++] = (char)ch;
    }
    out[used] = '\0';
    return true;
}

bool xhs_setup_form_ok(const xhs_setup_form_t *form)
{
    if (!form) return false;
    size_t ssid_len = strlen(form->ssid);
    if (ssid_len == 0 || ssid_len > 32) return false;
    if (strlen(form->password) > 64) return false;
    if (form->base_url[0] == '\0') return true;
    return xhs_http_base_ok(form->base_url);
}

bool xhs_parse_setup_form(const char *body, size_t len, xhs_setup_form_t *out)
{
    if (!out) return false;
    if (!body || len > XHS_SETUP_BODY_MAX) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    memset(out, 0, sizeof(*out));
    bool saw_ssid = false;
    size_t i = 0;
    while (i < len) {
        size_t start = i;
        while (i < len && body[i] != '&') i++;
        size_t part_len = i - start;
        const char *eq = NULL;
        for (size_t k = 0; k < part_len; k++) {
            if (body[start + k] == '=') {
                eq = body + start + k;
                break;
            }
        }
        if (eq) {
            char key[16];
            size_t key_len = (size_t)(eq - (body + start));
            size_t val_len = part_len - key_len - 1;
            if (url_decode(body + start, key_len, key, sizeof(key))) {
                char *dest = NULL;
                size_t cap = 0;
                if (strcmp(key, "ssid") == 0) {
                    dest = out->ssid;
                    cap = sizeof(out->ssid);
                    saw_ssid = true;
                } else if (strcmp(key, "password") == 0) {
                    dest = out->password;
                    cap = sizeof(out->password);
                } else if (strcmp(key, "base") == 0) {
                    dest = out->base_url;
                    cap = sizeof(out->base_url);
                }
                if (dest && !url_decode(eq + 1, val_len, dest, cap)) {
                    memset(out, 0, sizeof(*out));
                    return false;
                }
            }
        }
        if (i < len && body[i] == '&') i++;
    }
    if (!saw_ssid || !xhs_setup_form_ok(out)) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    return true;
}

static bool parse_json_string(const char *value, char *dst, size_t dst_len)
{
    if (!value || *value != '"' || !dst || dst_len == 0) return false;
    size_t used = 0;
    const char *p = value + 1;
    while (*p && *p != '"') {
        char ch;
        if (*p == '\\') {
            p++;
            if (*p == '"' || *p == '\\' || *p == '/') ch = *p;
            else return false;
            p++;
        } else {
            ch = *p++;
        }
        if (used + 1 >= dst_len) return false;
        dst[used++] = ch;
    }
    if (*p != '"') return false;
    dst[used] = '\0';
    return true;
}

bool xhs_parse_pair_reply(const char *json, xhs_pair_reply_t *out)
{
    if (!json || !out) return false;
    memset(out, 0, sizeof(*out));
    const char *status = find_value(json, "status");
    if (!status) return false;
    char text[16];
    if (!parse_json_string(status, text, sizeof(text))) return false;
    if (strcmp(text, "pending") == 0) return true;
    if (strcmp(text, "confirmed") != 0) return false;
    out->confirmed = true;
    const char *token = find_value(json, "token");
    if (token && *token == '"') {
        if (!parse_json_string(token, out->token, sizeof(out->token))) return false;
    }
    const char *base = find_value(json, "base");
    if (base && *base == '"') {
        if (!parse_json_string(base, out->base_url, sizeof(out->base_url))) return false;
    }
    return true;
}

static void clear_edges(xhs_prov_t *prov)
{
    prov->write_wifi = false;
    prov->write_token = false;
    prov->clear_net = false;
}

static void copy_cfg_field(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    dst[0] = '\0';
    if (!src) return;
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void xhs_prov_init(xhs_prov_t *prov, const xhs_net_cfg_t *saved, uint32_t code_seed)
{
    if (!prov) return;
    memset(prov, 0, sizeof(*prov));
    if (saved) prov->saved = *saved;
    if (prov->saved.ssid[0] == '\0') {
        prov->phase = XHS_PROV_AP;
        return;
    }
    if (xhs_config_usable(prov->saved.ssid, prov->saved.password,
                          prov->saved.base_url, prov->saved.token)) {
        prov->phase = XHS_PROV_READY;
        return;
    }
    prov->phase = XHS_PROV_PAIR;
    prov->broadcast = prov->saved.base_url[0] == '\0';
    xhs_pair_code_from_seed(prov->pair_code, code_seed);
}

bool xhs_prov_submit(xhs_prov_t *prov, const xhs_setup_form_t *form)
{
    if (!prov) return false;
    clear_edges(prov);
    if (!xhs_setup_form_ok(form)) return false;
    prov->attempt = *form;
    prov->has_attempt = true;
    prov->phase = XHS_PROV_JOINING;
    return true;
}

void xhs_prov_join_failed(xhs_prov_t *prov)
{
    if (!prov) return;
    clear_edges(prov);
    memset(&prov->attempt, 0, sizeof(prov->attempt));
    prov->has_attempt = false;
    prov->phase = XHS_PROV_AP;
}

void xhs_prov_join_ok(xhs_prov_t *prov)
{
    if (!prov || !prov->has_attempt) return;
    clear_edges(prov);
    bool url_changed = strcmp(prov->saved.base_url, prov->attempt.base_url) != 0;
    copy_cfg_field(prov->saved.ssid, sizeof(prov->saved.ssid), prov->attempt.ssid);
    copy_cfg_field(prov->saved.password, sizeof(prov->saved.password), prov->attempt.password);
    copy_cfg_field(prov->saved.base_url, sizeof(prov->saved.base_url), prov->attempt.base_url);
    if (url_changed || prov->saved.base_url[0] == '\0') prov->saved.token[0] = '\0';
    memset(&prov->attempt, 0, sizeof(prov->attempt));
    prov->has_attempt = false;
    prov->write_wifi = true;
    if (xhs_config_usable(prov->saved.ssid, prov->saved.password,
                          prov->saved.base_url, prov->saved.token)) {
        prov->phase = XHS_PROV_READY;
        prov->broadcast = false;
        return;
    }
    prov->phase = XHS_PROV_PAIR;
    prov->broadcast = prov->saved.base_url[0] == '\0';
    prov->pair_code[0] = '\0';
}

bool xhs_prov_apply_reply(xhs_prov_t *prov, const xhs_pair_reply_t *reply)
{
    if (!prov) return false;
    clear_edges(prov);
    if (!reply || prov->phase != XHS_PROV_PAIR || !reply->confirmed) return false;
    if (!ascii_token(reply->token)) return false;
    char base[XHS_BASE_CAP];
    memset(base, 0, sizeof(base));
    bool take_base = prov->saved.base_url[0] == '\0' && xhs_http_base_ok(reply->base_url);
    if (take_base) copy_cfg_field(base, sizeof(base), reply->base_url);
    const char *base_for_check = take_base ? base : prov->saved.base_url;
    if (!xhs_config_usable(prov->saved.ssid, prov->saved.password,
                           base_for_check, reply->token)) {
        return false;
    }
    copy_cfg_field(prov->saved.token, sizeof(prov->saved.token), reply->token);
    if (take_base) {
        copy_cfg_field(prov->saved.base_url, sizeof(prov->saved.base_url), base);
        prov->write_wifi = true;
    }
    prov->write_token = true;
    prov->phase = XHS_PROV_READY;
    prov->broadcast = false;
    return true;
}

void xhs_prov_reconfigure(xhs_prov_t *prov)
{
    if (!prov) return;
    clear_edges(prov);
    memset(prov->saved.ssid, 0, sizeof(prov->saved.ssid));
    memset(prov->saved.password, 0, sizeof(prov->saved.password));
    memset(prov->saved.base_url, 0, sizeof(prov->saved.base_url));
    memset(prov->saved.token, 0, sizeof(prov->saved.token));
    memset(&prov->attempt, 0, sizeof(prov->attempt));
    prov->has_attempt = false;
    prov->broadcast = false;
    prov->pair_code[0] = '\0';
    prov->phase = XHS_PROV_AP;
    prov->clear_net = true;
    prov->stats_kept = true;
}
