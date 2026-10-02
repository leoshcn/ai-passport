#include "xhs_ui.h"

#include "lvgl.h"

#include <stdio.h>
#include <string.h>

#define COL_BG 0x161114
#define COL_CARD 0x2C2226
#define COL_INK 0xF6F1EC
#define COL_MUTED 0xB7A79C
#define COL_RED 0xFF2442
#define COL_UP 0x7DCEA0
#define COL_DOWN 0xE07A7A

static lv_obj_t *s_screen;
static lv_obj_t *s_battery;
static lv_image_dsc_t s_avatar_dsc;
static uint8_t s_avatar_px[XHS_AVATAR_BYTES];
/* 拷到 RAM 再挂 Montserrat 回退，这样中文句子里的数字不会变成缺字框。不改 Flash 里的原描述符。 */
static lv_font_t s_text_font;
static lv_font_t s_cjk_fallback;

LV_FONT_DECLARE(xhs_font_16);

static lv_obj_t *screen_new(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(COL_INK), 0);
    lv_obj_set_style_text_font(screen, &s_text_font, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    return screen;
}

static void add_battery(lv_obj_t *screen)
{
    s_battery = lv_label_create(screen);
    lv_obj_set_style_text_font(s_battery, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_battery, lv_color_hex(COL_MUTED), 0);
    lv_label_set_text(s_battery, "");
    lv_obj_align(s_battery, LV_ALIGN_TOP_RIGHT, -16, 12);
}

static void show_screen(lv_obj_t *screen)
{
    lv_obj_t *previous = s_screen;
    s_screen = screen;
    lv_screen_load(screen);
    if (previous) lv_obj_delete(previous);
}

static lv_obj_t *card(lv_obj_t *screen, int x, int y, int w, int h, bool selected)
{
    lv_obj_t *obj = lv_obj_create(screen);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, 16, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, selected ? 2 : 0, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(COL_RED), 0);
    return obj;
}

static void label_at(lv_obj_t *parent, int x, int y, const char *text,
                     const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
}

static void metric(lv_obj_t *screen, int y, const char *title, const char *value, uint32_t color)
{
    lv_obj_t *obj = card(screen, 16, y, 208, 58, false);
    label_at(obj, 12, 4, title, &s_text_font, COL_MUTED);
    label_at(obj, 12, 24, value, &lv_font_montserrat_28, color);
}

static void show_failure(void)
{
    s_battery = NULL;
    lv_obj_t *screen = screen_new();
    add_battery(screen);
    lv_obj_t *label = lv_label_create(screen);
    lv_obj_set_width(label, 196);
    lv_obj_set_style_text_font(label, &s_text_font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(COL_INK), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, "配置失败，请重新刷新固件。");
    lv_obj_center(label);
    show_screen(screen);
}

static void show_dashboard(const xhs_stats_t *stats, bool has_stats,
                           const uint8_t *avatar, bool has_avatar,
                           bool update_failed)
{
    s_battery = NULL;
    lv_obj_t *screen = screen_new();
    add_battery(screen);

    if (has_avatar && avatar) {
        memcpy(s_avatar_px, avatar, sizeof(s_avatar_px));
        memset(&s_avatar_dsc, 0, sizeof(s_avatar_dsc));
        s_avatar_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        s_avatar_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        s_avatar_dsc.header.w = XHS_AVATAR_W;
        s_avatar_dsc.header.h = XHS_AVATAR_H;
        s_avatar_dsc.header.stride = XHS_AVATAR_W * 2;
        s_avatar_dsc.data_size = XHS_AVATAR_BYTES;
        s_avatar_dsc.data = s_avatar_px;

        lv_obj_t *image = lv_image_create(screen);
        lv_image_set_src(image, &s_avatar_dsc);
        lv_obj_set_size(image, XHS_AVATAR_W, XHS_AVATAR_H);
        lv_obj_set_pos(image, 16, 18);
        lv_obj_set_style_radius(image, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_clip_corner(image, true, 0);
    } else {
        lv_obj_t *mark = card(screen, 16, 18, XHS_AVATAR_W, XHS_AVATAR_H, false);
        lv_obj_set_style_radius(mark, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(mark, lv_color_hex(COL_RED), 0);
        char initial[8];
        const char *text = (has_stats && xhs_utf8_first(stats->username, initial, sizeof(initial)))
            ? initial : "";
        lv_obj_t *letter = lv_label_create(mark);
        lv_obj_set_style_text_font(letter, &s_text_font, 0);
        lv_obj_set_style_text_color(letter, lv_color_hex(COL_INK), 0);
        lv_label_set_text(letter, text);
        lv_obj_center(letter);
    }

    const char *name = (has_stats && stats->username[0]) ? stats->username : "尚未更新";
    lv_obj_t *name_label = lv_label_create(screen);
    lv_obj_set_width(name_label, 108);
    lv_obj_set_style_text_font(name_label, &s_text_font, 0);
    lv_obj_set_style_text_color(name_label, lv_color_hex(COL_INK), 0);
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(name_label, name);
    lv_obj_set_pos(name_label, 74, 30);

    char followers[16];
    char likes[16];
    char net[16];
    uint32_t net_color = COL_INK;
    if (!has_stats) {
        snprintf(followers, sizeof(followers), "--");
        snprintf(likes, sizeof(likes), "--");
        snprintf(net, sizeof(net), "--");
    } else {
        xhs_format_count(followers, sizeof(followers), stats->followers);
        xhs_format_count(likes, sizeof(likes), stats->likes_collects);
        xhs_format_net(net, sizeof(net), stats->net_followers_7d);
        if (stats->net_followers_7d > 0) net_color = COL_UP;
        else if (stats->net_followers_7d < 0) net_color = COL_DOWN;
    }

    metric(screen, 84, "粉丝", followers, COL_RED);
    metric(screen, 148, "获赞与收藏", likes, COL_INK);
    metric(screen, 212, "近7日净涨粉", net, net_color);

    char when[48];
    uint32_t when_color = COL_MUTED;
    if (update_failed && has_stats) {
        snprintf(when, sizeof(when), "更新失败");
        when_color = COL_DOWN;
    } else if (!has_stats || !stats->fetched_at[0]) {
        snprintf(when, sizeof(when), "尚未更新");
    } else {
        snprintf(when, sizeof(when), "更新于 %s", stats->fetched_at);
    }
    label_at(screen, 16, 278, when, &s_text_font, when_color);
    show_screen(screen);
}

static void show_provision(xhs_prov_phase_t phase)
{
    s_battery = NULL;
    lv_obj_t *screen = screen_new();
    add_battery(screen);
    if (phase == XHS_PROV_JOINING) {
        label_at(screen, 16, 140, "正在连接", &s_text_font, COL_INK);
    } else {
        label_at(screen, 16, 96, "连接热点", &s_text_font, COL_INK);
        label_at(screen, 16, 132, "Passport-Setup", &lv_font_montserrat_20, COL_RED);
        label_at(screen, 16, 176, "192.168.4.1", &lv_font_montserrat_20, COL_INK);
    }
    show_screen(screen);
}

static void show_pairing(const char *pair_code)
{
    s_battery = NULL;
    lv_obj_t *screen = screen_new();
    add_battery(screen);
    label_at(screen, 16, 96, "配对码", &s_text_font, COL_MUTED);
    label_at(screen, 16, 140, (pair_code && pair_code[0]) ? pair_code : "----",
             &lv_font_montserrat_28, COL_RED);
    show_screen(screen);
}

static void setting_row(lv_obj_t *screen, int y, const char *name, const char *value,
                        bool selected, bool editing)
{
    lv_obj_t *obj = card(screen, 16, y, 208, 46, selected);
    lv_obj_t *name_label = lv_label_create(obj);
    lv_obj_set_style_text_font(name_label, &s_text_font, 0);
    lv_obj_set_style_text_color(name_label, lv_color_hex(COL_INK), 0);
    lv_label_set_text(name_label, name);
    lv_obj_align(name_label, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_t *value_label = lv_label_create(obj);
    lv_obj_set_style_text_font(value_label, &s_text_font, 0);
    lv_obj_set_style_text_color(value_label,
                                lv_color_hex(selected && editing ? COL_RED : COL_MUTED), 0);
    lv_label_set_text(value_label, value);
    lv_obj_align(value_label, LV_ALIGN_RIGHT_MID, -12, 0);
}

static void show_settings(const xhs_settings_t *settings)
{
    s_battery = NULL;
    lv_obj_t *screen = screen_new();
    add_battery(screen);
    label_at(screen, 16, 28, "设置", &s_text_font, COL_INK);

    bool editing = settings->mode == XHS_MODE_EDIT;
    /* 四行高 46、间距 6，从 y=56 排到末行 y=212，末行底边 y=258。 */
    setting_row(screen, 56, "自动更新",
                settings->auto_update ? "开" : "关",
                settings->selected == XHS_ITEM_AUTO, editing);
    setting_row(screen, 108, "更新频率",
                settings->period == XHS_PERIOD_HOUR ? "每小时" : "每天",
                settings->selected == XHS_ITEM_PERIOD, editing);
    setting_row(screen, 160, "立即刷新", "",
                settings->selected == XHS_ITEM_REFRESH, editing);
    setting_row(screen, 212, "重新配网",
                editing && settings->selected == XHS_ITEM_REPROVISION ? "确认" : "进入",
                settings->selected == XHS_ITEM_REPROVISION, editing);

    const char *hint = "长按确认键返回";
    if (editing && settings->selected == XHS_ITEM_REPROVISION) hint = "确认键完成";
    else if (editing && settings->selected != XHS_ITEM_REFRESH) hint = "上下键调整，确认键完成";
    label_at(screen, 16, 268, hint, &s_text_font, COL_MUTED);
    show_screen(screen);
}

void xhs_ui_init(void)
{
    s_screen = NULL;
    s_battery = NULL;
    /* 界面文案用子集字库。用户名先落到内置的 1000 字回退，再落到 Montserrat，缺字保持方框。 */
    s_cjk_fallback = lv_font_source_han_sans_sc_16_cjk;
    s_cjk_fallback.fallback = &lv_font_montserrat_14;
    s_text_font = xhs_font_16;
    s_text_font.fallback = &s_cjk_fallback;
}

void xhs_ui_set_battery(int soc_percent)
{
    if (!s_battery) return;
    if (soc_percent < 0) lv_label_set_text(s_battery, "");
    else lv_label_set_text_fmt(s_battery, "%d%%", soc_percent);
}

void xhs_ui_show(xhs_view_t view, const xhs_settings_t *settings,
                 const xhs_stats_t *stats, bool has_stats,
                 const uint8_t *avatar, bool has_avatar,
                 xhs_prov_phase_t phase, const char *pair_code,
                 bool update_failed)
{
    if (view == XHS_VIEW_SETTINGS && settings) show_settings(settings);
    else if (view == XHS_VIEW_DASHBOARD) {
        show_dashboard(stats, has_stats, avatar, has_avatar, update_failed);
    } else if (view == XHS_VIEW_PROVISION) show_provision(phase);
    else if (view == XHS_VIEW_PAIRING) show_pairing(pair_code);
    else show_failure();
}
