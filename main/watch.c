/*
 * watch.c — a tiny smartwatch shell.
 *
 *   - HOME is an analog watchface (moving hour/minute/second hands + date).
 *   - A long-press on the watchface opens an APP DRAWER.
 *   - The drawer holds the "TamaWatchy" app (the dolphin virtual pet); tapping
 *     it launches the app. A long-press anywhere goes back one level
 *     (app -> watchface, drawer -> watchface).
 *
 * Screens are built on the active LVGL screen; transitions tear down the
 * previous screen's timers/objects first. Everything runs on the LVGL task.
 */
#include "watch.h"
#include "clock.h"
#include "tamagotchi.h"
#include <stdio.h>
#include <math.h>
#include <time.h>
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "esp_log.h"
#include "ad_monitor.h"

static const char *TAG = "watch";
#define PI 3.14159265f

/* analog face geometry (172 x 320 screen) */
#define CX 86
#define CY 120
#define R_FACE 80


enum { SCR_WATCH = 0, SCR_DRAWER, SCR_TAMA };
static int s_cur = -1;

static lv_obj_t  *s_scr;
static lv_timer_t *s_wf_timer;
static lv_obj_t  *s_hand_h, *s_hand_m, *s_hand_s, *s_date;
static lv_point_t s_ph[2], s_pm[2], s_ps[2];
static lv_point_t s_tick[12][2];
static void build_watchface(void);
static void build_drawer(void);

/* ---- navigation ------------------------------------------------------- */
static void teardown(void)
{
    if (s_cur == SCR_WATCH) {
        if (s_wf_timer) { lv_timer_del(s_wf_timer); s_wf_timer = NULL; }
        clock_persist();
    } else if (s_cur == SCR_TAMA) {
        tamagotchi_close();
    }
    lv_obj_clean(s_scr);
}

static void go_watch_cb(lv_event_t *e) { LV_UNUSED(e); teardown(); build_watchface(); }
static void go_drawer_cb(lv_event_t *e) { LV_UNUSED(e); teardown(); build_drawer(); }

static void open_tama_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    teardown();
    s_cur = SCR_TAMA;
    ESP_LOGI(TAG, "launching TamaWatchy");
    tamagotchi_open();
}

void watch_go_home(void)
{
    teardown();
    build_watchface();
}

/* the screen-level long-press only matters inside an app (its own objects
 * aren't a full-screen clickable root, so the press reaches the screen) */
static void screen_longpress_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (s_cur == SCR_TAMA) {
        ESP_LOGI(TAG, "long-press: exit app to watchface");
        watch_go_home();
    }
}



/* ---- analog watchface ------------------------------------------------- */
static void set_hand(lv_point_t *pts, lv_obj_t *line, float deg, int len, int tail)
{
    float r = deg * (PI / 180.0f);
    float dx = sinf(r), dy = -cosf(r);
    pts[0].x = CX - (int)lroundf(tail * dx);
    pts[0].y = CY - (int)lroundf(tail * dy);
    pts[1].x = CX + (int)lroundf(len * dx);
    pts[1].y = CY + (int)lroundf(len * dy);
    lv_line_set_points(line, pts, 2);
}

static void watchface_update(lv_timer_t *t)
{
    LV_UNUSED(t);
    time_t n = (time_t)clock_now();
    struct tm tm;
    localtime_r(&n, &tm);

    float ha = ((tm.tm_hour % 12) + tm.tm_min / 60.0f) * 30.0f;
    float ma = (tm.tm_min + tm.tm_sec / 60.0f) * 6.0f;
    float sa = tm.tm_sec * 6.0f;
    set_hand(s_ph, s_hand_h, ha, 46, 14);
    set_hand(s_pm, s_hand_m, ma, 66, 16);
    set_hand(s_ps, s_hand_s, sa, 72, 18);

    char buf[24];
    strftime(buf, sizeof(buf), "%a  %d %b", &tm);
    lv_label_set_text(s_date, buf);

    static int persist_acc = 0;
    if (++persist_acc >= 60) { persist_acc = 0; clock_persist(); }
}

static lv_obj_t *new_hand(lv_obj_t *parent, uint32_t color, int w)
{
    lv_obj_t *l = lv_line_create(parent);
    lv_obj_set_style_line_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_line_width(l, w, 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

static void build_watchface(void)
{
    s_cur = SCR_WATCH;
    ESP_LOGI(TAG, "watchface");

    lv_obj_t *root = lv_obj_create(s_scr);
    lv_obj_set_size(root, 172, 320);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x0A1A2A), 0);
    lv_obj_set_style_bg_grad_color(root, lv_color_hex(0x03101D), 0);
    lv_obj_set_style_bg_grad_dir(root, LV_GRAD_DIR_VER, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(root, go_drawer_cb, LV_EVENT_LONG_PRESSED, NULL);

    /* dial */
    lv_obj_t *face = lv_obj_create(root);
    lv_obj_set_size(face, 2 * R_FACE, 2 * R_FACE);
    lv_obj_set_pos(face, CX - R_FACE, CY - R_FACE);
    lv_obj_set_style_radius(face, R_FACE, 0);
    lv_obj_set_style_bg_color(face, lv_color_hex(0x10283E), 0);
    lv_obj_set_style_border_color(face, lv_color_hex(0x4A7AA8), 0);
    lv_obj_set_style_border_width(face, 3, 0);
    lv_obj_clear_flag(face, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(face, LV_OBJ_FLAG_SCROLLABLE);

    /* hour ticks */
    for (int i = 0; i < 12; i++) {
        float a = i * 30.0f * (PI / 180.0f);
        float dx = sinf(a), dy = -cosf(a);
        int outer = R_FACE - 4;
        int inner = (i % 3 == 0) ? R_FACE - 16 : R_FACE - 10;
        s_tick[i][0].x = CX + (int)lroundf(inner * dx);
        s_tick[i][0].y = CY + (int)lroundf(inner * dy);
        s_tick[i][1].x = CX + (int)lroundf(outer * dx);
        s_tick[i][1].y = CY + (int)lroundf(outer * dy);
        lv_obj_t *tk = lv_line_create(root);
        lv_obj_set_style_line_color(tk, lv_color_hex(0x9FC4E2), 0);
        lv_obj_set_style_line_width(tk, (i % 3 == 0) ? 3 : 1, 0);
        lv_obj_clear_flag(tk, LV_OBJ_FLAG_CLICKABLE);
        lv_line_set_points(tk, s_tick[i], 2);
    }

    s_hand_h = new_hand(root, 0xFFFFFF, 5);
    s_hand_m = new_hand(root, 0xFFFFFF, 3);
    s_hand_s = new_hand(root, 0xFF5C5C, 2);

    lv_obj_t *hub = lv_obj_create(root);
    lv_obj_set_size(hub, 10, 10);
    lv_obj_set_pos(hub, CX - 5, CY - 5);
    lv_obj_set_style_radius(hub, 5, 0);
    lv_obj_set_style_bg_color(hub, lv_color_hex(0xFF5C5C), 0);
    lv_obj_set_style_border_width(hub, 0, 0);
    lv_obj_clear_flag(hub, LV_OBJ_FLAG_CLICKABLE);

    s_date = lv_label_create(root);
    lv_obj_set_style_text_font(s_date, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_date, lv_color_hex(0xCDEBFF), 0);
    lv_obj_align(s_date, LV_ALIGN_TOP_MID, 0, 224);

    lv_obj_t *hint = lv_label_create(root);
    lv_label_set_text(hint, "hold for apps");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_8, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x6F8AA0), 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 286);

    lv_obj_t *batt = lv_label_create(root);
    char buf[12];
    sprintf(buf, "Bat: %dV", get_ad_voltage());
    lv_label_set_text(batt, buf);
    lv_obj_set_style_text_font(batt, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(batt, lv_color_hex(0x6F8AA0), 0);
    lv_obj_align(batt, LV_ALIGN_TOP_MID, 0, 6);

    s_wf_timer = lv_timer_create(watchface_update, 1000, NULL);
    watchface_update(NULL);
}

/* ---- app drawer ------------------------------------------------------- */
static void build_drawer(void)
{
    s_cur = SCR_DRAWER;
    ESP_LOGI(TAG, "app drawer");

    lv_obj_t *root = lv_obj_create(s_scr);
    lv_obj_set_size(root, 172, 320);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x0E1726), 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(root, go_watch_cb, LV_EVENT_LONG_PRESSED, NULL);

    lv_obj_t *title = lv_label_create(root);
    lv_label_set_text(title, "Apps");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 18);

    /* TamaWatchy app tile */
    lv_obj_t *tile = lv_btn_create(root);
    lv_obj_set_size(tile, 116, 116);
    lv_obj_align(tile, LV_ALIGN_TOP_MID, 0, 64);
    lv_obj_set_style_radius(tile, 22, 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(0x2E78C8), 0);
    lv_obj_set_style_bg_grad_color(tile, lv_color_hex(0x1B4E86), 0);
    lv_obj_set_style_bg_grad_dir(tile, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_shadow_width(tile, 0, 0);
    lv_obj_add_event_cb(tile, open_tama_cb, LV_EVENT_CLICKED, NULL);

    /* a little dolphin glyph: body arc + dorsal + tail */
    lv_obj_t *body = lv_arc_create(tile);
    lv_obj_set_size(body, 78, 78);
    lv_obj_center(body);
    lv_arc_set_bg_angles(body, 200, 340);
    lv_arc_set_value(body, 0);
    lv_obj_remove_style(body, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(body, lv_color_hex(0xE7F1F8), LV_PART_MAIN);
    lv_obj_set_style_arc_width(body, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_color(body, lv_color_hex(0xE7F1F8), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(body, 14, LV_PART_INDICATOR);

    lv_obj_t *name = lv_label_create(root);
    lv_label_set_text(name, "Tamagotchi");
    lv_obj_set_style_text_font(name, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(name, lv_color_hex(0xCDEBFF), 0);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 192);

    lv_obj_t *hint = lv_label_create(root);
    lv_label_set_text(hint, "tap to open  -  hold to go back");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_8, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x6F8AA0), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -24);
}

/* ---- entry ------------------------------------------------------------ */
void watch_start(void)
{
    clock_boot_init();
    s_scr = lv_scr_act();
    lv_obj_add_event_cb(s_scr, screen_longpress_cb, LV_EVENT_LONG_PRESSED, NULL);
    /* TODO: Add event callbacks for other input events */
    build_watchface();

}



