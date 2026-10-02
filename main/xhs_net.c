#include "xhs_net.h"

#include "xhs_store.h"

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include <string.h>
#include <time.h>

static const char *TAG = "xhs_net";
static const char *AP_SSID = "Passport-Setup";
static const uint16_t DISCOVER_PORT = 8788;

static QueueHandle_t s_results;
static TaskHandle_t s_task;
static esp_netif_t *s_sta;
static esp_netif_t *s_ap;
static httpd_handle_t s_httpd;
static TaskHandle_t s_dns_task;
static volatile bool s_dns_stop;
static int s_udp = -1;

static volatile bool s_got_ip;
static volatile bool s_sta_fail;
static volatile bool s_join_watch;
static bool s_sta_reconnect;
static bool s_wifi_inited;
static bool s_wifi_started;
static bool s_ap_services;
static TickType_t s_join_deadline;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static xhs_schedule_t s_sched;
static bool s_fetch_now;
static bool s_sched_changed;
static volatile bool s_reprovision;
static volatile int s_portal_state;
static TickType_t s_ap_hold_until;
static bool s_form_ready;
static xhs_setup_form_t s_form;

static xhs_prov_t s_prov;
static xhs_net_cfg_t s_boot;
static uint32_t s_pair_seed;
static xhs_fetch_result_t s_outgoing;

static const char PORTAL_PAGE[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Passport-Setup</title></head><body>"
    "<form method=\"post\" action=\"/setup\">"
    "<p>Wi-Fi<br><input name=\"ssid\" maxlength=\"32\" required></p>"
    "<p>密码<br><input name=\"password\" type=\"password\" maxlength=\"64\"></p>"
    "<p>后端地址，可留空<br><input name=\"base\" maxlength=\"120\" "
    "placeholder=\"http://192.168.1.10:8787\"></p>"
    "<p><button type=\"submit\">连接</button></p>"
    "</form></body></html>";

static void publish(const xhs_fetch_result_t *result)
{
    if (!s_results || !result) return;
    (void)xQueueSend(s_results, result, pdMS_TO_TICKS(200));
}

static void publish_phase(void)
{
    memset(&s_outgoing, 0, sizeof(s_outgoing));
    s_outgoing.kind = XHS_RESULT_PHASE;
    s_outgoing.phase = s_prov.phase;
    memcpy(s_outgoing.pair_code, s_prov.pair_code, sizeof(s_outgoing.pair_code));
    publish(&s_outgoing);
    ESP_LOGI(TAG, "phase %d", (int)s_prov.phase);
}

static void persist_edges(void)
{
    if (s_prov.clear_net) {
        if (xhs_store_clear_provision() != ESP_OK) ESP_LOGW(TAG, "provision was not cleared");
        s_prov.clear_net = false;
    }
    if (s_prov.write_wifi) {
        if (xhs_store_save_wifi(s_prov.saved.ssid, s_prov.saved.password,
                                s_prov.saved.base_url) != ESP_OK) {
            ESP_LOGW(TAG, "wifi was not saved");
        } else if (s_prov.saved.token[0] == '\0' && !s_prov.write_token) {
            /* URL 变了或还没确认时，内存里的令牌已清空。不把 NVS 里的旧令牌留下，
             * 否则重启会把新地址和旧令牌当成已配对。 */
            if (xhs_store_save_token("") != ESP_OK) {
                ESP_LOGW(TAG, "token was not cleared");
            }
        }
        s_prov.write_wifi = false;
    }
    if (s_prov.write_token) {
        if (xhs_store_save_token(s_prov.saved.token) != ESP_OK) {
            ESP_LOGW(TAG, "token was not saved");
        }
        s_prov.write_token = false;
    }
}

static void log_heap(const char *where)
{
    ESP_LOGI(TAG, "%s free=%u largest=%u", where,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

static void on_disconnect(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    uint8_t reason = 0;
    if (data) reason = ((const wifi_event_sta_disconnected_t *)data)->reason;
    ESP_LOGI(TAG, "sta disconnect %u", (unsigned)reason);
    s_got_ip = false;
    if (s_join_watch) {
        if (xhs_sta_disconnect_is_final(reason)) s_sta_fail = true;
        else esp_wifi_connect();
        return;
    }
    if (s_sta_reconnect) esp_wifi_connect();
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    s_got_ip = true;
    s_sta_fail = false;
}

static void on_ap_sta(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    ESP_LOGI(TAG, "ap station joined");
}

static void on_ap_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    ESP_LOGI(TAG, "ap dhcp assigned");
}

static esp_err_t ensure_wifi(void)
{
    if (s_wifi_inited) return ESP_OK;
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_WIFI_STA();
    s_sta = esp_netif_new(&cfg);
    if (!s_sta) return ESP_ERR_NO_MEM;
    err = esp_netif_attach_wifi_station(s_sta);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_default_wifi_sta_handlers();
    if (err != ESP_OK) return err;

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_cfg);
    if (err != ESP_OK) return err;
    err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_disconnect, NULL);
    if (err != ESP_OK) return err;
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL);
    if (err != ESP_OK) return err;
    err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, on_ap_sta, NULL);
    if (err != ESP_OK) return err;
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, on_ap_ip, NULL);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) return err;
    s_wifi_inited = true;
    log_heap("wifi init");
    return ESP_OK;
}

static esp_err_t ap_netif_open(void)
{
    if (s_ap) return ESP_OK;
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_WIFI_AP();
    s_ap = esp_netif_new(&cfg);
    if (!s_ap) return ESP_ERR_NO_MEM;
    esp_err_t err = esp_netif_attach_wifi_ap(s_ap);
    if (err != ESP_OK) return err;
    return esp_wifi_set_default_wifi_ap_handlers();
}

static esp_err_t apply_sta_config(const char *ssid, const char *password)
{
    wifi_config_t sta;
    memset(&sta, 0, sizeof(sta));
    size_t ssid_n = ssid ? strlen(ssid) : 0;
    size_t pass_n = password ? strlen(password) : 0;
    if (ssid_n >= sizeof(sta.sta.ssid)) ssid_n = sizeof(sta.sta.ssid) - 1;
    if (pass_n >= sizeof(sta.sta.password)) pass_n = sizeof(sta.sta.password) - 1;
    if (ssid && ssid_n) memcpy(sta.sta.ssid, ssid, ssid_n);
    if (password && pass_n) memcpy(sta.sta.password, password, pass_n);
    sta.sta.threshold.authmode = pass_n == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    sta.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    return esp_wifi_set_config(WIFI_IF_STA, &sta);
}

static uint32_t ap_ipv4(void)
{
    esp_netif_ip_info_t info;
    if (s_ap && esp_netif_get_ip_info(s_ap, &info) == ESP_OK && info.ip.addr != 0) {
        return info.ip.addr;
    }
    return htonl(0xC0A80401);
}

static void dns_reply(int sock, const uint8_t *req, int len, const struct sockaddr_in *from)
{
    if (len < 12 || len > 256) return;
    if (((req[4] << 8) | req[5]) != 1) return;
    int end = 12;
    while (end < len && req[end] != 0) {
        int label = req[end];
        if ((label & 0xC0) == 0xC0) {
            end += 2;
            break;
        }
        if (label <= 0 || end + 1 + label >= len) return;
        end += 1 + label;
    }
    if (end >= len || req[end] != 0) return;
    end += 5;
    if (end > len || end + 16 > 320) return;
    uint8_t resp[320];
    memcpy(resp, req, (size_t)end);
    resp[2] = 0x81;
    resp[3] = 0x80;
    resp[6] = 0;
    resp[7] = 1;
    resp[8] = resp[9] = resp[10] = resp[11] = 0;
    int at = end;
    resp[at++] = 0xC0;
    resp[at++] = 0x0C;
    resp[at++] = 0;
    resp[at++] = 1;
    resp[at++] = 0;
    resp[at++] = 1;
    resp[at++] = 0;
    resp[at++] = 0;
    resp[at++] = 0;
    resp[at++] = 30;
    resp[at++] = 0;
    resp[at++] = 4;
    uint32_t ip = ap_ipv4();
    memcpy(resp + at, &ip, 4);
    at += 4;
    (void)sendto(sock, resp, (size_t)at, 0, (const struct sockaddr *)from, sizeof(*from));
}

static void dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "provision dns failed");
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    int yes = 1;
    (void)setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(sock);
        ESP_LOGE(TAG, "provision dns failed");
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    while (!s_dns_stop) {
        uint8_t buf[256];
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        if (n > 0) dns_reply(sock, buf, n, &from);
    }
    close(sock);
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t dns_start(void)
{
    if (s_dns_task) return ESP_OK;
    s_dns_stop = false;
    if (xTaskCreate(dns_task, "xhs_dns", 4096, NULL, 3, &s_dns_task) != pdPASS) {
        s_dns_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void dns_stop(void)
{
    if (!s_dns_task) return;
    s_dns_stop = true;
    for (int i = 0; i < 20 && s_dns_task; i++) vTaskDelay(pdMS_TO_TICKS(100));
}

static esp_err_t send_html(httpd_req_t *req, const char *page)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, page);
}

static esp_err_t status_get(httpd_req_t *req)
{
    int state = s_portal_state;
    if (state == 3) {
        return send_html(req,
            "<!DOCTYPE html><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            "<p>已连上家里的 Wi-Fi。请断开本热点，看设备屏幕上的配对码。</p>");
    }
    if (state == 2) {
        return send_html(req,
            "<!DOCTYPE html><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            "<p>没连上。请检查名称和密码后再试。</p>"
            "<p><a href=\"/\">返回</a></p>");
    }
    return send_html(req,
        "<!DOCTYPE html><meta charset=\"utf-8\">"
        "<meta http-equiv=\"refresh\" content=\"2;url=/status\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<p>正在连接</p>");
}

static esp_err_t portal_get(httpd_req_t *req)
{
    if (strncmp(req->uri, "/status", 7) == 0) return status_get(req);
    return send_html(req, PORTAL_PAGE);
}

static esp_err_t setup_post(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > XHS_SETUP_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid");
        return ESP_FAIL;
    }
    char body[XHS_SETUP_BODY_MAX + 1];
    int total = 0;
    int retries = 0;
    while (total < req->content_len && retries < 4) {
        int got = httpd_req_recv(req, body + total, (size_t)(req->content_len - total));
        if (got == HTTPD_SOCK_ERR_TIMEOUT) {
            retries++;
            continue;
        }
        if (got <= 0) {
            memset(body, 0, sizeof(body));
            return ESP_FAIL;
        }
        total += got;
    }
    if (total != req->content_len) {
        memset(body, 0, sizeof(body));
        httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "invalid");
        return ESP_FAIL;
    }
    body[total] = '\0';
    xhs_setup_form_t form;
    bool ok = xhs_parse_setup_form(body, (size_t)total, &form);
    memset(body, 0, sizeof(body));
    if (!ok) {
        memset(&form, 0, sizeof(form));
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid");
        return ESP_OK;
    }
    taskENTER_CRITICAL(&s_mux);
    s_form = form;
    s_form_ready = true;
    taskEXIT_CRITICAL(&s_mux);
    memset(&form, 0, sizeof(form));
    s_portal_state = 1;
    if (s_task) xTaskNotifyGive(s_task);
    return send_html(req,
        "<!DOCTYPE html><meta charset=\"utf-8\">"
        "<meta http-equiv=\"refresh\" content=\"2;url=/status\">"
        "<p>正在连接</p>");
}

static esp_err_t http_start(void)
{
    if (s_httpd) return ESP_OK;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.ctrl_port = 32768;
    cfg.max_open_sockets = 3;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 5;
    cfg.send_wait_timeout = 5;
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 8;
    cfg.backlog_conn = 2;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) {
        s_httpd = NULL;
        return err;
    }
    httpd_uri_t get_uri = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = portal_get,
    };
    httpd_uri_t post_uri = {
        .uri = "/setup",
        .method = HTTP_POST,
        .handler = setup_post,
    };
    err = httpd_register_uri_handler(s_httpd, &get_uri);
    if (err == ESP_OK) err = httpd_register_uri_handler(s_httpd, &post_uri);
    if (err != ESP_OK) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    return err;
}

static void http_stop(void)
{
    if (!s_httpd) return;
    httpd_stop(s_httpd);
    s_httpd = NULL;
}

static void provision_services_stop(void)
{
    http_stop();
    dns_stop();
    if (s_wifi_started) {
        (void)esp_wifi_set_mode(WIFI_MODE_STA);
        (void)esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    }
    s_ap_services = false;
    log_heap("provision stop");
}

static esp_err_t provision_start(void)
{
    esp_err_t err = ensure_wifi();
    if (err != ESP_OK) return err;
    err = ap_netif_open();
    if (err != ESP_OK) return err;
    wifi_config_t ap;
    memset(&ap, 0, sizeof(ap));
    size_t ssid_n = strlen(AP_SSID);
    memcpy(ap.ap.ssid, AP_SSID, ssid_n);
    ap.ap.ssid_len = (uint8_t)ssid_n;
    ap.ap.channel = 1;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ap.ap.max_connection = 2;
    ap.ap.beacon_interval = 100;
    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (err != ESP_OK) return err;
    if (!s_wifi_started) {
        err = esp_wifi_start();
        if (err != ESP_OK) return err;
        s_wifi_started = true;
    }
    (void)esp_wifi_set_ps(WIFI_PS_NONE);
    err = dns_start();
    if (err != ESP_OK) return err;
    err = http_start();
    if (err != ESP_OK) {
        dns_stop();
        return err;
    }
    s_ap_services = true;
    log_heap("provision start");
    return ESP_OK;
}

static esp_err_t station_start(const char *ssid, const char *password, bool reconnect)
{
    esp_err_t err = ensure_wifi();
    if (err != ESP_OK) return err;
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) return err;
    err = apply_sta_config(ssid, password);
    if (err != ESP_OK) return err;
    if (!s_wifi_started) {
        err = esp_wifi_start();
        if (err != ESP_OK) return err;
        s_wifi_started = true;
    }
    (void)esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    s_sta_reconnect = reconnect;
    s_join_watch = false;
    return esp_wifi_connect();
}

static void udp_close(void)
{
    if (s_udp >= 0) {
        close(s_udp);
        s_udp = -1;
    }
}

static bool udp_open(void)
{
    if (s_udp >= 0) return true;
    s_udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_udp < 0) return false;
    int yes = 1;
    (void)setsockopt(s_udp, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = htons(0);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s_udp, (struct sockaddr *)&local, sizeof(local)) != 0) {
        udp_close();
        return false;
    }
    struct timeval tv = {.tv_sec = 0, .tv_usec = 200000};
    (void)setsockopt(s_udp, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return true;
}

static bool sync_clock(int64_t *now_unix)
{
    static bool started;
    if (!started) {
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        if (esp_netif_sntp_init(&config) != ESP_OK) return false;
        started = true;
    }
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(8000)) != ESP_OK) return false;
    time_t now = time(NULL);
    if (now < 1700000000) return false;
    *now_unix = (int64_t)now;
    return true;
}

static bool read_body(esp_http_client_handle_t client, uint8_t *buf, size_t cap, size_t *used, bool text)
{
    size_t n = 0;
    while (n < cap) {
        int got = esp_http_client_read(client, (char *)buf + n, cap - n);
        if (got < 0) return false;
        if (got == 0) break;
        n += (size_t)got;
    }
    if (n == cap) {
        char extra;
        int more = esp_http_client_read(client, &extra, 1);
        if (more > 0) return false;
    }
    if (text) {
        if (n >= cap) return false;
        buf[n] = 0;
    }
    *used = n;
    return true;
}

static bool http_get(const char *url, const char *token, uint8_t *buf, size_t cap, size_t *used, bool text)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 15000,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;
    bool ok = false;
    if (token && token[0] &&
        esp_http_client_set_header(client, "X-Device-Token", token) != ESP_OK) {
        goto done;
    }
    if (esp_http_client_open(client, 0) != ESP_OK) {
        ESP_LOGW(TAG, "http open failed");
        goto done;
    }
    (void)esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGW(TAG, "http status %d", status);
        goto close_client;
    }
    ok = read_body(client, buf, cap, used, text);
    if (!ok) ESP_LOGW(TAG, "http body rejected");
close_client:
    esp_http_client_close(client);
done:
    esp_http_client_cleanup(client);
    return ok;
}

static xhs_session_seen_t s_session_seen;
static TickType_t s_next_session;
static bool s_fetch_aligned;

static bool creator_session_changed(bool align_saved)
{
    char url[XHS_URL_CAP];
    char revision[XHS_SESSION_REV_CAP];
    uint8_t body[96];
    size_t used = 0;
    if (!xhs_join_url(s_prov.saved.base_url, "/api/v1/session", url, sizeof(url))) return false;
    if (!http_get(url, s_prov.saved.token, body, sizeof(body), &used, true)) return false;
    bool parsed = xhs_parse_session_revision((char *)body, revision, sizeof(revision));
    memset(body, 0, sizeof(body));
    if (!parsed) return false;
    return xhs_note_session_revision(&s_session_seen, revision, align_saved);
}

static bool fetch_once(void)
{
    char stats_url[XHS_URL_CAP];
    char avatar_url[XHS_URL_CAP];
    memset(&s_outgoing, 0, sizeof(s_outgoing));
    if (!xhs_join_url(s_prov.saved.base_url, "/api/v1/stats", stats_url, sizeof(stats_url)) ||
        !xhs_join_url(s_prov.saved.base_url, "/api/v1/avatar", avatar_url, sizeof(avatar_url))) {
        publish(&s_outgoing);
        return false;
    }

    static uint8_t body[1536];
    size_t used = 0;
    if (!http_get(stats_url, s_prov.saved.token, body, sizeof(body), &used, true) ||
        !xhs_parse_stats((char *)body, &s_outgoing.stats)) {
        memset(body, 0, sizeof(body));
        publish(&s_outgoing);
        return false;
    }
    memset(body, 0, sizeof(body));

    size_t avatar_n = 0;
    if (http_get(avatar_url, s_prov.saved.token, s_outgoing.avatar, sizeof(s_outgoing.avatar),
                 &avatar_n, false) &&
        avatar_n == XHS_AVATAR_BYTES) {
        s_outgoing.has_avatar = true;
    }
    s_outgoing.ok = true;
    s_outgoing.kind = XHS_RESULT_FETCH;
    s_fetch_aligned = true;
    publish(&s_outgoing);
    taskENTER_CRITICAL(&s_mux);
    s_sched.last_unix = s_outgoing.stats.fetched_unix;
    s_sched.has_last = true;
    taskEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "stats updated");
    return true;
}

static void accept_pairing_buffer(char *buf)
{
    xhs_pair_reply_t reply;
    bool parsed = xhs_parse_pair_reply(buf, &reply);
    memset(buf, 0, 512);
    if (!parsed) return;
    bool accepted = xhs_prov_apply_reply(&s_prov, &reply);
    memset(&reply, 0, sizeof(reply));
    if (!accepted) return;
    persist_edges();
    udp_close();
    s_fetch_now = true;
    ESP_LOGI(TAG, "pairing confirmed");
    publish_phase();
}

static void pair_step(void)
{
    if (s_prov.pair_code[0] == '\0') {
        xhs_pair_code_from_seed(s_prov.pair_code, esp_random());
        publish_phase();
    }
    char buf[512];
    memset(buf, 0, sizeof(buf));
    if (s_prov.broadcast) {
        if (!udp_open()) return;
        char hello[32];
        if (!xhs_format_pair_hello(s_prov.pair_code, hello, sizeof(hello))) return;
        struct sockaddr_in dest;
        memset(&dest, 0, sizeof(dest));
        dest.sin_family = AF_INET;
        dest.sin_port = htons(DISCOVER_PORT);
        dest.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        (void)sendto(s_udp, hello, strlen(hello), 0, (struct sockaddr *)&dest, sizeof(dest));
        esp_netif_ip_info_t info;
        if (s_sta && esp_netif_get_ip_info(s_sta, &info) == ESP_OK && info.netmask.addr != 0) {
            dest.sin_addr.s_addr = info.ip.addr | ~info.netmask.addr;
            (void)sendto(s_udp, hello, strlen(hello), 0, (struct sockaddr *)&dest, sizeof(dest));
        }
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(s_udp, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &from_len);
        if (n <= 0) {
            memset(buf, 0, sizeof(buf));
            return;
        }
        buf[n] = '\0';
        accept_pairing_buffer(buf);
        return;
    }

    char path[48];
    char url[XHS_URL_CAP];
    snprintf(path, sizeof(path), "/api/v1/pair?code=%s", s_prov.pair_code);
    if (!xhs_join_url(s_prov.saved.base_url, path, url, sizeof(url))) return;
    size_t used = 0;
    if (!http_get(url, NULL, (uint8_t *)buf, sizeof(buf), &used, true)) {
        memset(buf, 0, sizeof(buf));
        return;
    }
    accept_pairing_buffer(buf);
}

static bool take_form(xhs_setup_form_t *form)
{
    bool ready = false;
    taskENTER_CRITICAL(&s_mux);
    if (s_form_ready) {
        *form = s_form;
        memset(&s_form, 0, sizeof(s_form));
        s_form_ready = false;
        ready = true;
    }
    taskEXIT_CRITICAL(&s_mux);
    return ready;
}

static void handle_form(void)
{
    xhs_setup_form_t form;
    if (!take_form(&form)) return;
    if (!s_ap_services && provision_start() != ESP_OK) {
        memset(&form, 0, sizeof(form));
        ESP_LOGE(TAG, "provision start failed");
        return;
    }
    if (!xhs_prov_submit(&s_prov, &form)) {
        memset(&form, 0, sizeof(form));
        return;
    }
    memset(&form, 0, sizeof(form));
    s_join_watch = true;
    s_sta_fail = false;
    s_got_ip = false;
    s_sta_reconnect = false;
    s_join_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(20000);
    if (apply_sta_config(s_prov.attempt.ssid, s_prov.attempt.password) != ESP_OK ||
        esp_wifi_connect() != ESP_OK) {
        s_sta_fail = true;
    }
    publish_phase();
}

static void finish_join(bool ok)
{
    s_join_watch = false;
    if (!ok) {
        s_sta_reconnect = false;
        s_portal_state = 2;
        xhs_prov_join_failed(&s_prov);
        if (s_wifi_started) esp_wifi_disconnect();
        ESP_LOGI(TAG, "provision join failed");
        publish_phase();
        return;
    }
    s_sta_reconnect = true;
    xhs_prov_join_ok(&s_prov);
    persist_edges();
    s_portal_state = 3;
    s_ap_hold_until = xTaskGetTickCount() + pdMS_TO_TICKS(15000);
    if (s_prov.phase == XHS_PROV_PAIR && s_prov.pair_code[0] == '\0') {
        xhs_pair_code_from_seed(s_prov.pair_code, esp_random());
    }
    if (s_prov.phase == XHS_PROV_READY) s_fetch_now = true;
    ESP_LOGI(TAG, "provision join ok");
    publish_phase();
}

static void handle_join(void)
{
    if (s_prov.phase != XHS_PROV_JOINING) return;
    if (s_got_ip) {
        finish_join(true);
        return;
    }
    bool timed_out = (int32_t)(xTaskGetTickCount() - s_join_deadline) >= 0;
    if (s_sta_fail || timed_out) finish_join(false);
}

static void handle_reprovision(void)
{
    bool requested = false;
    taskENTER_CRITICAL(&s_mux);
    if (s_reprovision) {
        s_reprovision = false;
        requested = true;
    }
    taskEXIT_CRITICAL(&s_mux);
    if (!requested) return;
    s_join_watch = false;
    s_sta_reconnect = false;
    s_fetch_now = false;
    udp_close();
    /* 先擦 NVS。擦除失败就保持当前配网，避免内存已空、分区里仍留着密码和令牌。 */
    if (xhs_store_clear_provision() != ESP_OK) {
        ESP_LOGW(TAG, "provision was not cleared");
        publish_phase();
        return;
    }
    xhs_prov_reconfigure(&s_prov);
    s_prov.clear_net = false;
    if (s_wifi_started) esp_wifi_disconnect();
    if (provision_start() != ESP_OK) ESP_LOGE(TAG, "provision start failed");
    publish_phase();
}

static xhs_schedule_t schedule_copy(void)
{
    xhs_schedule_t copy;
    taskENTER_CRITICAL(&s_mux);
    copy = s_sched;
    taskEXIT_CRITICAL(&s_mux);
    return copy;
}

static void net_task(void *arg)
{
    (void)arg;
    xhs_prov_init(&s_prov, &s_boot, s_pair_seed);
    memset(&s_boot, 0, sizeof(s_boot));
    if (s_prov.phase == XHS_PROV_AP) {
        if (provision_start() != ESP_OK) ESP_LOGE(TAG, "provision start failed");
    } else if (station_start(s_prov.saved.ssid, s_prov.saved.password, true) != ESP_OK) {
        ESP_LOGE(TAG, "wifi start failed");
    }
    publish_phase();

    bool holding = false;
    TickType_t hold_until = 0;
    bool announced_fail = false;
    TickType_t next_pair = 0;

    for (;;) {
        handle_reprovision();
        handle_form();
        handle_join();
        if (s_ap_services && s_portal_state == 3 &&
            (int32_t)(xTaskGetTickCount() - s_ap_hold_until) >= 0) {
            provision_services_stop();
        }

        if (s_prov.phase == XHS_PROV_AP && !s_ap_services) {
            static TickType_t retry_at;
            TickType_t now_tick = xTaskGetTickCount();
            if ((int32_t)(now_tick - retry_at) >= 0) {
                if (provision_start() != ESP_OK) {
                    ESP_LOGE(TAG, "provision start failed");
                    retry_at = now_tick + pdMS_TO_TICKS(3000);
                }
            }
        }

        taskENTER_CRITICAL(&s_mux);
        bool manual = s_fetch_now;
        s_fetch_now = false;
        bool sched_changed = s_sched_changed;
        s_sched_changed = false;
        taskEXIT_CRITICAL(&s_mux);
        if (sched_changed) holding = false;

        if (s_prov.phase == XHS_PROV_PAIR) {
            TickType_t now_tick = xTaskGetTickCount();
            if (s_got_ip && (int32_t)(now_tick - next_pair) >= 0) {
                pair_step();
                next_pair = xTaskGetTickCount() + pdMS_TO_TICKS(3000);
            }
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
            continue;
        }
        if (s_prov.phase != XHS_PROV_READY) {
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
            continue;
        }

        xhs_schedule_t sched = schedule_copy();
        TickType_t now_tick = xTaskGetTickCount();
        bool due = manual;
        TickType_t wait = pdMS_TO_TICKS(5000);
        if (!manual) {
            if (holding) {
                due = (int32_t)(now_tick - hold_until) >= 0;
                if (!due) wait = hold_until - now_tick;
            } else if (sched.auto_update) {
                if (!s_got_ip) {
                    due = !sched.has_last;
                    wait = pdMS_TO_TICKS(15000);
                } else {
                    int64_t now_unix = 0;
                    bool time_ok = sync_clock(&now_unix);
                    int64_t seconds = xhs_seconds_until_due(
                        true, sched.period, now_unix, time_ok, sched.last_unix, sched.has_last);
                    if (seconds <= 0) due = true;
                    else {
                        uint64_t ms = (uint64_t)seconds * 1000ULL;
                        if (ms > 3600ULL * 1000ULL) ms = 3600ULL * 1000ULL;
                        wait = pdMS_TO_TICKS(ms);
                    }
                }
            }
        }

        if (!due && s_got_ip && (int32_t)(now_tick - s_next_session) >= 0) {
            s_next_session = now_tick + pdMS_TO_TICKS(XHS_SESSION_POLL_SEC * 1000);
            if (creator_session_changed(sched.has_last && !s_fetch_aligned)) due = true;
        }

        if (due) {
            bool fetched = false;
            if (s_wifi_started) {
                int waits = 0;
                while (!s_got_ip && waits < 30 && !s_reprovision) {
                    vTaskDelay(pdMS_TO_TICKS(500));
                    waits++;
                }
                fetched = s_got_ip && !s_reprovision && fetch_once();
            }
            if (!fetched) {
                if (!announced_fail) {
                    if (!s_wifi_started || !s_got_ip) {
                        memset(&s_outgoing, 0, sizeof(s_outgoing));
                        publish(&s_outgoing);
                    }
                    announced_fail = true;
                }
                holding = true;
                hold_until = xTaskGetTickCount() + pdMS_TO_TICKS(XHS_FETCH_RETRY_SEC * 1000);
            } else {
                announced_fail = false;
                holding = sched.auto_update;
                hold_until = xTaskGetTickCount() + pdMS_TO_TICKS(xhs_period_seconds(sched.period) * 1000);
            }
        }

        if (!manual && !holding && !sched.auto_update) wait = portMAX_DELAY;
        {
            const TickType_t session_slice = pdMS_TO_TICKS(XHS_SESSION_POLL_SEC * 1000);
            if (wait > session_slice) wait = session_slice;
        }
        (void)ulTaskNotifyTake(pdTRUE, wait);
    }
}

void xhs_net_request_fetch(void)
{
    taskENTER_CRITICAL(&s_mux);
    s_fetch_now = true;
    taskEXIT_CRITICAL(&s_mux);
    if (s_task) xTaskNotifyGive(s_task);
}

void xhs_net_request_reprovision(void)
{
    taskENTER_CRITICAL(&s_mux);
    s_reprovision = true;
    taskEXIT_CRITICAL(&s_mux);
    if (s_task) xTaskNotifyGive(s_task);
}

void xhs_net_set_schedule(bool auto_update, xhs_period_t period, int64_t last_unix, bool has_last)
{
    taskENTER_CRITICAL(&s_mux);
    s_sched.auto_update = auto_update;
    s_sched.period = period;
    s_sched.last_unix = last_unix;
    s_sched.has_last = has_last;
    s_sched_changed = true;
    taskEXIT_CRITICAL(&s_mux);
    if (s_task) xTaskNotifyGive(s_task);
}

esp_err_t xhs_net_start(QueueHandle_t results, const xhs_schedule_t *initial,
                        const xhs_net_cfg_t *saved, uint32_t pair_seed)
{
    if (!results || !initial || !saved) return ESP_ERR_INVALID_ARG;
    s_results = results;
    s_sched = *initial;
    s_boot = *saved;
    s_pair_seed = pair_seed;
    if (xTaskCreate(net_task, "xhs_net", 12288, NULL, 3, &s_task) != pdPASS) {
        s_task = NULL;
        memset(&s_boot, 0, sizeof(s_boot));
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
