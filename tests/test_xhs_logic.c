#include "xhs_logic.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifdef __TINYC__
/* This Windows TinyCC declares strtoll and leaves the symbol in libmingwex, which is not shipped. */
long long _strtoi64(const char *nptr, char **endptr, int base);
long long strtoll(const char *nptr, char **endptr, int base)
{
    return _strtoi64(nptr, endptr, base);
}
#endif

static char *read_fixture(char *buf, size_t cap)
{
    FILE *file = fopen("tests/fixtures/xhs_stats.json", "rb");
    assert(file);
    size_t n = fread(buf, 1, cap - 1, file);
    fclose(file);
    assert(n > 0);
    buf[n] = '\0';
    return buf;
}

static void test_format(void)
{
    char out[32];
    xhs_format_count(out, sizeof(out), 49);
    assert(strcmp(out, "49") == 0);
    xhs_format_count(out, sizeof(out), 1043);
    assert(strcmp(out, "1,043") == 0);
    xhs_format_count(out, sizeof(out), 1234567);
    assert(strcmp(out, "1,234,567") == 0);
    xhs_format_count(out, sizeof(out), -1200);
    assert(strcmp(out, "-1,200") == 0);
    xhs_format_net(out, sizeof(out), 4);
    assert(strcmp(out, "+4") == 0);
    xhs_format_net(out, sizeof(out), 0);
    assert(strcmp(out, "0") == 0);
    xhs_format_net(out, sizeof(out), -4);
    assert(strcmp(out, "-4") == 0);
    xhs_format_net(out, sizeof(out), 1043);
    assert(strcmp(out, "+1,043") == 0);
}

static void test_config_and_url(void)
{
    assert(!xhs_config_usable("", "", "http://10.0.0.8:8787", "token-ok-1"));
    assert(!xhs_config_usable("home", "pw", "https://10.0.0.8", "token-ok-1"));
    assert(!xhs_config_usable("home", "pw", "http://10.0.0.8:8787", "short"));
    assert(!xhs_config_usable("home", NULL, "http://10.0.0.8:8787", "token-ok-1"));
    char long_pw[80];
    memset(long_pw, 'a', 65);
    long_pw[65] = '\0';
    assert(!xhs_config_usable("home", long_pw, "http://10.0.0.8:8787", "token-ok-1"));
    assert(xhs_config_usable("home", "", "http://10.0.0.8:8787", "token-ok-1"));

    char url[XHS_URL_CAP];
    assert(xhs_join_url("http://10.0.0.8:8787/", "/api/v1/stats", url, sizeof(url)));
    assert(strcmp(url, "http://10.0.0.8:8787/api/v1/stats") == 0);
    assert(!xhs_join_url("http://10.0.0.8", "api", url, sizeof(url)));
}

static void test_schedule(void)
{
    assert(xhs_period_seconds(XHS_PERIOD_HOUR) == 3600);
    assert(xhs_period_seconds(XHS_PERIOD_DAY) == 86400);
    assert(xhs_seconds_until_due(false, XHS_PERIOD_HOUR, 100, true, 90, true) == -1);
    assert(xhs_seconds_until_due(true, XHS_PERIOD_HOUR, 100, false, 90, true) == 0);
    assert(xhs_seconds_until_due(true, XHS_PERIOD_HOUR, 100, true, 0, false) == 0);
    assert(xhs_seconds_until_due(true, XHS_PERIOD_HOUR, 2000, true, 1000, true) == 2600);
    assert(xhs_seconds_until_due(true, XHS_PERIOD_DAY, 90000, true, 1000, true) == 0);
    assert(xhs_seconds_until_due(true, XHS_PERIOD_HOUR, 500, true, 1000, true) == 0);

    xhs_session_seen_t seen;
    memset(&seen, 0, sizeof(seen));
    char revision[XHS_SESSION_REV_CAP];
    assert(xhs_parse_session_revision("{\"revision\":\"0123456789abcdef\"}", revision, sizeof(revision)));
    assert(strcmp(revision, "0123456789abcdef") == 0);
    assert(xhs_parse_session_revision("{\"revision\":\"\"}", revision, sizeof(revision)));
    assert(revision[0] == '\0');
    assert(!xhs_parse_session_revision("{\"revision\":\"nope\"}", revision, sizeof(revision)));
    assert(!xhs_note_session_revision(&seen, "0123456789abcdef", false));
    assert(!xhs_note_session_revision(&seen, "0123456789abcdef", true));
    assert(xhs_note_session_revision(&seen, "fedcba9876543210", false));
    assert(!xhs_note_session_revision(&seen, "", false));
    assert(xhs_note_session_revision(&seen, "0123456789abcdef", false));
    xhs_session_seen_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    assert(xhs_note_session_revision(&fresh, "0123456789abcdef", true));
    assert(!xhs_note_session_revision(NULL, "0123456789abcdef", true));
}

static void test_buttons(void)
{
    xhs_app_t app;
    xhs_app_init(&app, false, XHS_PROV_AP, false, true, XHS_PERIOD_DAY);
    assert(app.view == XHS_VIEW_FAILURE);
    xhs_app_handle(&app, XHS_EVENT_OK_LONG);
    assert(app.view == XHS_VIEW_FAILURE);
    assert(!app.request_fetch);

    xhs_app_init(&app, true, XHS_PROV_AP, false, true, XHS_PERIOD_DAY);
    assert(app.view == XHS_VIEW_PROVISION);
    assert(!app.config_ok);
    xhs_app_handle(&app, XHS_EVENT_OK_CLICK);
    assert(!app.request_fetch);
    xhs_app_handle(&app, XHS_EVENT_OK_LONG);
    assert(app.view == XHS_VIEW_SETTINGS);
    xhs_app_handle(&app, XHS_EVENT_OK_LONG);
    assert(app.view == XHS_VIEW_PROVISION);

    xhs_app_init(&app, true, XHS_PROV_READY, true, true, XHS_PERIOD_DAY);
    assert(app.view == XHS_VIEW_DASHBOARD);
    assert(app.config_ok);
    xhs_app_handle(&app, XHS_EVENT_OK_CLICK);
    assert(app.request_fetch);
    xhs_app_handle(&app, XHS_EVENT_OK_LONG);
    assert(app.view == XHS_VIEW_SETTINGS);
    assert(app.settings.mode == XHS_MODE_NAV);
    assert(app.settings.selected == XHS_ITEM_AUTO);

    xhs_app_handle(&app, XHS_EVENT_DOWN_CLICK);
    assert(app.settings.selected == XHS_ITEM_PERIOD);
    xhs_app_handle(&app, XHS_EVENT_UP_CLICK);
    assert(app.settings.selected == XHS_ITEM_AUTO);
    xhs_app_handle(&app, XHS_EVENT_OK_CLICK);
    assert(app.settings.mode == XHS_MODE_EDIT);
    assert(app.settings.auto_update);
    xhs_app_handle(&app, XHS_EVENT_DOWN_CLICK);
    assert(!app.settings.auto_update);
    xhs_app_handle(&app, XHS_EVENT_OK_CLICK);
    assert(app.save_settings);
    assert(app.settings.mode == XHS_MODE_NAV);

    xhs_app_handle(&app, XHS_EVENT_DOWN_CLICK);
    assert(app.settings.selected == XHS_ITEM_PERIOD);
    xhs_app_handle(&app, XHS_EVENT_OK_CLICK);
    xhs_app_handle(&app, XHS_EVENT_UP_CLICK);
    assert(app.settings.period == XHS_PERIOD_HOUR);
    xhs_app_handle(&app, XHS_EVENT_OK_LONG);
    assert(app.view == XHS_VIEW_DASHBOARD);
    assert(app.save_settings);

    xhs_app_handle(&app, XHS_EVENT_FETCH_FAIL);
    assert(app.view == XHS_VIEW_DASHBOARD);
    assert(app.update_failed);
    assert(app.has_stats);
    app.has_stats = false;
    xhs_app_handle(&app, XHS_EVENT_FETCH_FAIL);
    assert(app.view == XHS_VIEW_FAILURE);
    xhs_app_handle(&app, XHS_EVENT_FETCH_OK);
    assert(app.has_stats);
    assert(!app.update_failed);
    assert(app.view == XHS_VIEW_DASHBOARD);

    xhs_app_handle(&app, XHS_EVENT_OK_LONG);
    xhs_app_handle(&app, XHS_EVENT_FETCH_FAIL);
    assert(app.view == XHS_VIEW_SETTINGS);
    assert(app.update_failed);
    assert(app.has_stats);

    xhs_app_handle(&app, XHS_EVENT_DOWN_CLICK);
    assert(app.settings.selected == XHS_ITEM_PERIOD);
    xhs_app_handle(&app, XHS_EVENT_DOWN_CLICK);
    assert(app.settings.selected == XHS_ITEM_REFRESH);
    xhs_app_handle(&app, XHS_EVENT_DOWN_CLICK);
    assert(app.settings.selected == XHS_ITEM_REPROVISION);
    bool auto_before = app.settings.auto_update;
    xhs_period_t period_before = app.settings.period;
    xhs_app_handle(&app, XHS_EVENT_OK_CLICK);
    assert(app.settings.mode == XHS_MODE_EDIT);
    xhs_app_handle(&app, XHS_EVENT_UP_CLICK);
    assert(app.settings.auto_update == auto_before);
    assert(app.settings.period == period_before);
    assert(!app.request_reprovision);
    xhs_app_handle(&app, XHS_EVENT_OK_CLICK);
    assert(app.request_reprovision);
    assert(app.view == XHS_VIEW_PROVISION);
    assert(!app.config_ok);

    xhs_app_init(&app, true, XHS_PROV_READY, true, true, XHS_PERIOD_HOUR);
    xhs_app_handle(&app, XHS_EVENT_OK_LONG);
    assert(app.view == XHS_VIEW_SETTINGS);
    assert(app.settings.selected == XHS_ITEM_AUTO);
    auto_before = app.settings.auto_update;
    period_before = app.settings.period;
    xhs_app_handle(&app, XHS_EVENT_DOWN_CLICK);
    xhs_app_handle(&app, XHS_EVENT_DOWN_CLICK);
    assert(app.settings.selected == XHS_ITEM_REFRESH);
    assert(app.settings.mode == XHS_MODE_NAV);
    xhs_app_handle(&app, XHS_EVENT_OK_LONG);
    assert(app.view == XHS_VIEW_DASHBOARD);
    assert(!app.request_fetch);
    assert(app.save_settings);
    assert(!app.request_reprovision);
    assert(app.settings.auto_update == auto_before);
    assert(app.settings.period == period_before);

    xhs_app_handle(&app, XHS_EVENT_OK_LONG);
    xhs_app_handle(&app, XHS_EVENT_DOWN_CLICK);
    xhs_app_handle(&app, XHS_EVENT_DOWN_CLICK);
    assert(app.settings.selected == XHS_ITEM_REFRESH);
    xhs_app_handle(&app, XHS_EVENT_OK_CLICK);
    assert(app.view == XHS_VIEW_DASHBOARD);
    assert(app.request_fetch);
    assert(!app.save_settings);
    assert(!app.request_reprovision);
    assert(app.settings.mode == XHS_MODE_NAV);
    assert(app.settings.auto_update == auto_before);
    assert(app.settings.period == period_before);
    assert(app.config_ok);

    xhs_app_init(&app, true, XHS_PROV_AP, false, false, XHS_PERIOD_DAY);
    assert(!app.config_ok);
    xhs_app_handle(&app, XHS_EVENT_OK_LONG);
    xhs_app_handle(&app, XHS_EVENT_DOWN_CLICK);
    xhs_app_handle(&app, XHS_EVENT_DOWN_CLICK);
    assert(app.settings.selected == XHS_ITEM_REFRESH);
    auto_before = app.settings.auto_update;
    period_before = app.settings.period;
    xhs_app_handle(&app, XHS_EVENT_OK_CLICK);
    assert(app.view == XHS_VIEW_DASHBOARD);
    assert(!app.request_fetch);
    assert(!app.request_reprovision);
    assert(!app.save_settings);
    assert(app.phase == XHS_PROV_AP);
    assert(!app.config_ok);
    assert(app.settings.mode == XHS_MODE_NAV);
    assert(app.settings.auto_update == auto_before);
    assert(app.settings.period == period_before);

    xhs_app_init(&app, true, XHS_PROV_READY, true, true, XHS_PERIOD_DAY);
    xhs_app_handle(&app, XHS_EVENT_OK_LONG);
    app.settings.selected = XHS_ITEM_REFRESH;
    app.settings.mode = XHS_MODE_EDIT;
    auto_before = app.settings.auto_update;
    period_before = app.settings.period;
    xhs_app_handle(&app, XHS_EVENT_UP_CLICK);
    xhs_app_handle(&app, XHS_EVENT_DOWN_CLICK);
    assert(app.view == XHS_VIEW_SETTINGS);
    assert(app.settings.selected == XHS_ITEM_REFRESH);
    assert(app.settings.mode == XHS_MODE_EDIT);
    assert(app.settings.auto_update == auto_before);
    assert(app.settings.period == period_before);
    assert(!app.request_fetch);
    xhs_app_handle(&app, XHS_EVENT_OK_CLICK);
    assert(app.view == XHS_VIEW_DASHBOARD);
    assert(app.request_fetch);
    assert(!app.save_settings);
    assert(!app.request_reprovision);
    assert(app.settings.mode == XHS_MODE_NAV);
    assert(app.settings.auto_update == auto_before);
    assert(app.settings.period == period_before);
}


static void test_parse(void)
{
    char fixture[512];
    read_fixture(fixture, sizeof(fixture));
    xhs_stats_t stats;
    assert(xhs_parse_stats(fixture, &stats));
    assert(strcmp(stats.username, "小红") == 0);
    assert(stats.followers == 49);
    assert(stats.likes_collects == 1043);
    assert(stats.net_followers_7d == 4);
    assert(strcmp(stats.fetched_at, "2026-09-30 17:40") == 0);
    assert(stats.fetched_unix == 1780000000);

    const char *escaped =
        "{\"ok\":true,\"username\":\"\\u5c0f\\u7ea2\","
        "\"followers\":1,\"likes_collects\":2,\"net_followers_7d\":-3,"
        "\"fetched_at\":\"2026-09-30 17:40\",\"fetched_unix\":1780000000}";
    assert(xhs_parse_stats(escaped, &stats));
    assert(strcmp(stats.username, "小红") == 0);
    assert(stats.net_followers_7d == -3);

    assert(!xhs_parse_stats("{\"ok\":false}", &stats));
    assert(stats.net_followers_7d == -3);

    char long_name[256];
    strcpy(long_name, "{\"ok\":true,\"username\":\"");
    size_t used = strlen(long_name);
    for (int i = 0; i < 40; i++) {
        memcpy(long_name + used, "测", 3);
        used += 3;
    }
    strcpy(long_name + used,
           "\",\"followers\":1,\"likes_collects\":2,\"net_followers_7d\":0,"
           "\"fetched_at\":\"t\",\"fetched_unix\":10}");
    assert(xhs_parse_stats(long_name, &stats));
    assert(strlen(stats.username) < XHS_USERNAME_CAP);
    assert((strlen(stats.username) % 3) == 0);

    char initial[8];
    assert(xhs_utf8_first("小红", initial, sizeof(initial)));
    assert(strcmp(initial, "小") == 0);
    assert(!xhs_utf8_first("", initial, sizeof(initial)));
}

static void fill_form(xhs_setup_form_t *form, const char *ssid, const char *password, const char *base)
{
    memset(form, 0, sizeof(*form));
    snprintf(form->ssid, sizeof(form->ssid), "%s", ssid);
    snprintf(form->password, sizeof(form->password), "%s", password);
    snprintf(form->base_url, sizeof(form->base_url), "%s", base);
}

static void test_provision(void)
{
    xhs_prov_t prov;
    xhs_prov_init(&prov, NULL, 7);
    assert(prov.phase == XHS_PROV_AP);
    assert(prov.saved.ssid[0] == '\0');
    assert(!prov.write_wifi);

    xhs_setup_form_t bad;
    fill_form(&bad, "new", "wrong-pass", "http://10.0.0.9:8787");
    xhs_net_cfg_t saved;
    memset(&saved, 0, sizeof(saved));
    snprintf(saved.ssid, sizeof(saved.ssid), "old");
    snprintf(saved.password, sizeof(saved.password), "old-pass");
    snprintf(saved.base_url, sizeof(saved.base_url), "http://10.0.0.8:8787");
    snprintf(saved.token, sizeof(saved.token), "token-ok-1");
    xhs_prov_init(&prov, &saved, 1);
    assert(prov.phase == XHS_PROV_READY);
    assert(xhs_prov_submit(&prov, &bad));
    assert(prov.phase == XHS_PROV_JOINING);
    assert(strcmp(prov.saved.password, "old-pass") == 0);
    assert(xhs_sta_disconnect_is_final(15));
    assert(xhs_sta_disconnect_is_final(202));
    assert(xhs_sta_disconnect_is_final(204));
    assert(!xhs_sta_disconnect_is_final(8));
    assert(!xhs_sta_disconnect_is_final(201));
    xhs_prov_join_failed(&prov);
    assert(prov.phase == XHS_PROV_AP);
    assert(strcmp(prov.saved.ssid, "old") == 0);
    assert(strcmp(prov.saved.password, "old-pass") == 0);
    assert(strcmp(prov.saved.token, "token-ok-1") == 0);
    assert(!prov.write_wifi);
    assert(prov.attempt.password[0] == '\0');

    xhs_setup_form_t open_net;
    fill_form(&open_net, "home", "pw", "");
    assert(xhs_prov_submit(&prov, &open_net));
    xhs_prov_join_ok(&prov);
    assert(prov.write_wifi);
    assert(prov.phase == XHS_PROV_PAIR);
    assert(prov.broadcast);
    assert(strcmp(prov.saved.ssid, "home") == 0);
    assert(strcmp(prov.saved.password, "pw") == 0);
    assert(prov.saved.token[0] == '\0');
    assert(prov.saved.base_url[0] == '\0');

    xhs_pair_reply_t early;
    memset(&early, 0, sizeof(early));
    early.confirmed = false;
    snprintf(early.token, sizeof(early.token), "token-ok-1");
    assert(!xhs_prov_apply_reply(&prov, &early));
    assert(prov.saved.token[0] == '\0');
    assert(prov.phase == XHS_PROV_PAIR);

    xhs_pair_code_from_seed(prov.pair_code, 9);
    xhs_pair_reply_t reply;
    memset(&reply, 0, sizeof(reply));
    reply.confirmed = true;
    snprintf(reply.token, sizeof(reply.token), "token-ok-1");
    snprintf(reply.base_url, sizeof(reply.base_url), "http://192.168.1.20:8787");
    assert(xhs_prov_apply_reply(&prov, &reply));
    assert(prov.phase == XHS_PROV_READY);
    assert(prov.write_token);
    assert(prov.write_wifi);
    assert(strcmp(prov.saved.token, "token-ok-1") == 0);
    assert(strcmp(prov.saved.base_url, "http://192.168.1.20:8787") == 0);

    xhs_prov_reconfigure(&prov);
    assert(prov.stats_kept);
    assert(prov.clear_net);
    assert(prov.phase == XHS_PROV_AP);
    assert(prov.saved.ssid[0] == '\0');
    assert(prov.saved.password[0] == '\0');
    assert(prov.saved.base_url[0] == '\0');
    assert(prov.saved.token[0] == '\0');

    xhs_setup_form_t direct;
    fill_form(&direct, "home", "", "http://10.0.0.8:8787");
    assert(xhs_prov_submit(&prov, &direct));
    xhs_prov_join_ok(&prov);
    assert(prov.phase == XHS_PROV_PAIR);
    assert(!prov.broadcast);
    memset(&reply, 0, sizeof(reply));
    reply.confirmed = true;
    snprintf(reply.token, sizeof(reply.token), "token-ok-2");
    assert(xhs_prov_apply_reply(&prov, &reply));
    assert(prov.phase == XHS_PROV_READY);
    assert(prov.write_token);
    assert(!prov.write_wifi);
    assert(strcmp(prov.saved.base_url, "http://10.0.0.8:8787") == 0);

    char code[5];
    xhs_pair_code_from_seed(code, 42);
    xhs_pair_code_from_seed(code, 42);
    assert(xhs_pair_code_ok(code));
    char again[5];
    xhs_pair_code_from_seed(again, 42);
    assert(strcmp(code, again) == 0);
    xhs_pair_code_from_seed(again, 99);
    assert(strlen(again) == 4);
    for (int i = 0; i < 4; i++) {
        assert(strchr("0O1IL", again[i]) == NULL);
    }
    char hello[32];
    assert(xhs_format_pair_hello(again, hello, sizeof(hello)));
    assert(!xhs_format_pair_hello("0O1I", hello, sizeof(hello)));

    xhs_setup_form_t parsed;
    const char *body = "ssid=my%20net&password=a%2Fb&base=";
    assert(xhs_parse_setup_form(body, strlen(body), &parsed));
    assert(strcmp(parsed.ssid, "my net") == 0);
    assert(strcmp(parsed.password, "a/b") == 0);
    assert(parsed.base_url[0] == '\0');
    char huge[XHS_SETUP_BODY_MAX + 8];
    memset(huge, 'a', sizeof(huge));
    huge[sizeof(huge) - 1] = '\0';
    assert(!xhs_parse_setup_form(huge, XHS_SETUP_BODY_MAX + 1, &parsed));
    assert(parsed.password[0] == '\0');
    const char *long_pass = "ssid=home&password="
        "01234567890123456789012345678901234567890123456789012345678901234";
    assert(!xhs_parse_setup_form(long_pass, strlen(long_pass), &parsed));

    const char *pending = "{\"ok\":true,\"status\":\"pending\",\"token\":\"token-ok-1\"}";
    xhs_pair_reply_t got;
    assert(xhs_parse_pair_reply(pending, &got));
    assert(!got.confirmed);
    const char *confirmed =
        "{\"ok\":true,\"status\":\"confirmed\",\"token\":\"token-ok-1\","
        "\"base\":\"http://192.168.1.20:8787\"}";
    assert(xhs_parse_pair_reply(confirmed, &got));
    assert(got.confirmed);
    assert(strcmp(got.token, "token-ok-1") == 0);
    assert(strcmp(got.base_url, "http://192.168.1.20:8787") == 0);
}

int main(void)
{
    test_format();
    test_config_and_url();
    test_schedule();
    test_buttons();
    test_parse();
    test_provision();
    return 0;
}
