/*
 * tamagotchi.c — the dolphin virtual-pet game.
 *
 * Features: animated dolphin, four decaying care stats, an overall level/XP,
 * four trainable skills, a live software clock, and a first-boot name picker.
 *
 * Screens (172 x 320 portrait):
 *   - NAME ENTRY (first boot only): arcade-style letter picker.
 *   - GAME: header (clock / name / level + XP) -> stat bars -> dolphin stage ->
 *     status line -> action buttons (Feed / Play / Sleep / Wash). The level
 *     chip opens an INFO panel (XP, settable clock, skills).
 *
 * Everything runs on the LVGL task (timers + input callbacks), single threaded.
 */
#include "tamagotchi.h"
#include "dolphin.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdint.h>

#include "lvgl.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "tamagotchi";

#define NVS_NS   "pet"
#define NVS_KEY  "state"
#define STATE_MAGIC 0xD0179604u          /* bumped: weight + poop fields    */

#define NAME_MAX 8

enum { SK_ACRO = 0, SK_APP, SK_GROOM, SK_STAM, SK_COUNT };
static const char *SKILL_NAMES[SK_COUNT] = { "Acrobatics", "Appetite", "Grooming", "Stamina" };

/* ---- persisted pet state ---------------------------------------------- */
typedef struct {
    uint32_t magic;
    char     name[12];
    uint8_t  hunger, happy, energy, hygiene, sleeping;
    uint8_t  _pad[3];
    uint32_t age_min;
    uint16_t level;
    uint8_t  strain;                      /* obesity strain 0..100 -> death  */
    uint8_t  dead;                        /* 0 alive, 1 dead                 */
    uint32_t xp;
    uint16_t skill_lvl[SK_COUNT];
    uint32_t skill_xp[SK_COUNT];
    uint8_t  weight;                      /* 0..100 fatness                  */
    uint8_t  feed_count;                  /* feeds since last poop           */
    uint8_t  poop;                        /* messes on the sea floor         */
    uint8_t  muscle;                      /* 0..100 fitness/"ripped" build   */
    int64_t  epoch;                       /* persisted wall-clock seconds    */
} pet_state_t;

static pet_state_t pet;

/* ---- ui handles (game) ------------------------------------------------ */
static lv_obj_t *scr;
static lv_obj_t *lbl_clock, *lbl_name, *lbl_level, *bar_xp;
static lv_obj_t *lbl_status;
static lv_obj_t *bar_food, *bar_fun, *bar_rest, *bar_clean;
static lv_obj_t *btn_sleep_lbl;

static lv_timer_t *g_anim = NULL, *g_logic = NULL;

/* info panel (created on demand) */
static lv_obj_t *panel = NULL;
static lv_obj_t *pnl_time, *pnl_date;

/* name entry */
static lv_obj_t *entry_root = NULL;
static lv_obj_t *entry_preview, *entry_letter;
static int       entry_idx = 0;
static char      entry_buf[NAME_MAX + 1];
static const char *CHARSET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ ";

/* ---- animation / bookkeeping ------------------------------------------ */
static uint32_t        frame;
static dolphin_mood_t  base_mood = DOLPHIN_MOOD_NORMAL;
static dolphin_mood_t  override_mood;
static int             override_frames;
static uint32_t        decay_acc;
static uint32_t        save_acc;
static char            flash_msg[28];
static int             flash_secs;          /* transient banner (level up)  */

static void start_game(void);
static void show_name_entry(void);
static void show_death(void);
static void stop_game_timers(void);

/* ---- clock ------------------------------------------------------------ */
static int64_t clock_default_epoch(void)
{
    struct tm t0 = {0};
    t0.tm_year = 2026 - 1900; t0.tm_mon = 5; t0.tm_mday = 27;
    t0.tm_hour = 12; t0.tm_min = 0; t0.tm_isdst = 0;
    return (int64_t)mktime(&t0);
}
static void clock_set(int64_t epoch)
{
    struct timeval tv = { .tv_sec = (time_t)epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);
}
static int64_t clock_now(void)
{
    time_t n; time(&n); return (int64_t)n;
}

/* ---- helpers ---------------------------------------------------------- */
static uint8_t clamp_add(uint8_t v, int delta)
{
    int n = (int)v + delta;
    if (n < 0) n = 0;
    if (n > 100) n = 100;
    return (uint8_t)n;
}

static uint32_t level_need(uint16_t lvl)  { return 40 + 30 * (uint32_t)lvl; }
static uint32_t skill_need(uint16_t lvl)  { return 20 + 15 * (uint32_t)lvl; }

static void set_flash(const char *msg)
{
    strncpy(flash_msg, msg, sizeof(flash_msg) - 1);
    flash_msg[sizeof(flash_msg) - 1] = 0;
    flash_secs = 3;
}

static void add_xp(uint32_t amt)
{
    pet.xp += amt;
    while (pet.xp >= level_need(pet.level)) {
        pet.xp -= level_need(pet.level);
        pet.level++;
        char m[28];
        snprintf(m, sizeof(m), "Level up!  Lv %u", pet.level);
        set_flash(m);
    }
}
static void add_skill_xp(int s, uint32_t amt)
{
    pet.skill_xp[s] += amt;
    while (pet.skill_xp[s] >= skill_need(pet.skill_lvl[s])) {
        pet.skill_xp[s] -= skill_need(pet.skill_lvl[s]);
        pet.skill_lvl[s]++;
        char m[28];
        snprintf(m, sizeof(m), "%s Lv %u!", SKILL_NAMES[s], pet.skill_lvl[s]);
        set_flash(m);
    }
}

static void pet_reset(const char *name)
{
    memset(&pet, 0, sizeof(pet));
    pet.magic = STATE_MAGIC;
    strncpy(pet.name, (name && name[0]) ? name : "Finn", sizeof(pet.name) - 1);
    pet.hunger = 80; pet.happy = 80; pet.energy = 90; pet.hygiene = 85;
    pet.sleeping = 0; pet.age_min = 0;
    pet.weight = 30; pet.feed_count = 0; pet.poop = 0; pet.muscle = 25;
    pet.strain = 0; pet.dead = 0;
    pet.level = 1; pet.xp = 0;
    for (int i = 0; i < SK_COUNT; i++) { pet.skill_lvl[i] = 1; pet.skill_xp[i] = 0; }
    pet.epoch = clock_default_epoch();
}

static bool pet_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t sz = sizeof(pet);
        pet_state_t tmp;
        if (nvs_get_blob(h, NVS_KEY, &tmp, &sz) == ESP_OK &&
            sz == sizeof(pet) && tmp.magic == STATE_MAGIC) {
            pet = tmp;
            nvs_close(h);
            ESP_LOGI(TAG, "loaded pet '%s' Lv%u", pet.name, pet.level);
            return true;
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "no saved pet");
    return false;
}

static void pet_save(void)
{
    pet.epoch = clock_now();
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, NVS_KEY, &pet, sizeof(pet));
        nvs_commit(h);
        nvs_close(h);
    }
}

static void set_override(dolphin_mood_t m, int frames)
{
    override_mood = m;
    override_frames = frames;
}

static dolphin_mood_t compute_base_mood(void)
{
    if (pet.sleeping) return DOLPHIN_MOOD_SLEEPING;
    if (pet.hunger == 0 && (pet.happy == 0 || pet.hygiene == 0)) return DOLPHIN_MOOD_SICK;
    if (pet.poop > 0) return DOLPHIN_MOOD_DIRTY;
    if (pet.hunger < 22) return DOLPHIN_MOOD_HUNGRY;
    if (pet.hygiene < 22) return DOLPHIN_MOOD_DIRTY;
    if (pet.happy < 22 || pet.energy < 18) return DOLPHIN_MOOD_SAD;
    if (pet.happy > 78 && pet.hunger > 55 && pet.hygiene > 55) return DOLPHIN_MOOD_HAPPY;
    return DOLPHIN_MOOD_NORMAL;
}

static void status_text(dolphin_mood_t m, char *out, size_t n)
{
    const char *s;
    switch (m) {
        case DOLPHIN_MOOD_SLEEPING: s = "is fast asleep  Zzz"; break;
        case DOLPHIN_MOOD_HUNGRY:   s = "is hungry!";          break;
        case DOLPHIN_MOOD_DIRTY:    s = "needs a wash";        break;
        case DOLPHIN_MOOD_SAD:      s = "feels down...";       break;
        case DOLPHIN_MOOD_SICK:     s = "feels sick :(";       break;
        case DOLPHIN_MOOD_EATING:   snprintf(out, n, "Yum, tasty fish!"); return;
        case DOLPHIN_MOOD_PLAYING:  snprintf(out, n, "Wheee! Splash!");   return;
        case DOLPHIN_MOOD_HAPPY:    s = "feels great!";        break;
        default:                    s = "is swimming along";   break;
    }
    snprintf(out, n, "%s %s", pet.name, s);
}

/* ---- info panel (forward) --------------------------------------------- */
static void panel_update(void);

static void refresh_ui(void)
{
    lv_bar_set_value(bar_food,  pet.hunger,  LV_ANIM_OFF);
    lv_bar_set_value(bar_fun,   pet.happy,   LV_ANIM_OFF);
    lv_bar_set_value(bar_rest,  pet.energy,  LV_ANIM_OFF);
    lv_bar_set_value(bar_clean, pet.hygiene, LV_ANIM_OFF);

    dolphin_set_fat(15 * pet.weight / 100);
    dolphin_set_muscle(11 * pet.muscle / 100);
    dolphin_set_poop(pet.poop);

    char buf[40];
    if (flash_secs > 0) {
        lv_label_set_text(lbl_status, flash_msg);
    } else if (override_frames == 0 && pet.weight >= 92) {
        snprintf(buf, sizeof(buf), "%s is dangerously obese!", pet.name);
        lv_label_set_text(lbl_status, buf);
    } else if (override_frames == 0 && pet.poop > 0) {
        snprintf(buf, sizeof(buf), "%s made a mess - wash it!", pet.name);
        lv_label_set_text(lbl_status, buf);
    } else {
        dolphin_mood_t shown = override_frames > 0 ? override_mood : base_mood;
        bool calm = (shown == DOLPHIN_MOOD_NORMAL || shown == DOLPHIN_MOOD_HAPPY);
        if (override_frames == 0 && calm && pet.muscle > 72) {
            snprintf(buf, sizeof(buf), "%s is looking ripped!", pet.name);
        } else if (override_frames == 0 && calm && pet.weight > 80) {
            snprintf(buf, sizeof(buf), "%s is looking chubby!", pet.name);
        } else {
            status_text(shown, buf, sizeof(buf));
        }
        lv_label_set_text(lbl_status, buf);
    }

    lv_label_set_text(lbl_name, pet.name);

    snprintf(buf, sizeof(buf), "Lv %u", pet.level);
    lv_label_set_text(lbl_level, buf);
    lv_bar_set_value(bar_xp, (int)(pet.xp * 100 / level_need(pet.level)), LV_ANIM_OFF);

    time_t n = (time_t)clock_now();
    struct tm tmv; localtime_r(&n, &tmv);
    char tbuf[8];
    strftime(tbuf, sizeof(tbuf), "%H:%M", &tmv);
    lv_label_set_text(lbl_clock, tbuf);

    lv_label_set_text(btn_sleep_lbl, pet.sleeping ? "Wake" : "Sleep");

    if (panel) panel_update();
}

/* ---- actions ---------------------------------------------------------- */
static void act_feed(void)
{
    if (pet.sleeping) pet.sleeping = 0;
    bool stuffed = (pet.hunger > 85);            /* feeding an already-full pet */
    pet.hunger = clamp_add(pet.hunger, 22 + 2 * pet.skill_lvl[SK_APP]);
    pet.happy  = clamp_add(pet.happy, stuffed ? -3 : 3);  /* gorging is unpleasant */
    pet.weight = clamp_add(pet.weight, stuffed ? 15 : 8); /* overfeeding packs it on */
    pet.muscle = clamp_add(pet.muscle, -1);      /* lounging softens it      */
    add_skill_xp(SK_APP, 8);
    add_xp(10);
    set_override(DOLPHIN_MOOD_EATING, 32);

    /* every few feeds the dolphin makes a mess */
    if (++pet.feed_count >= 3) {
        pet.feed_count = 0;
        if (pet.poop < 60) pet.poop++;           /* piles up, no real limit */
        dolphin_poop_drop();
        pet.hygiene = clamp_add(pet.hygiene, -15);
        pet.happy   = clamp_add(pet.happy, -4);
        pet.weight  = clamp_add(pet.weight, -4);
    }
    pet_save();
}

static void act_play(void)
{
    if (pet.sleeping) return;
    if (pet.energy < 15) { set_override(DOLPHIN_MOOD_SAD, 12); return; }
    int cost = 12 - pet.skill_lvl[SK_ACRO] / 2; if (cost < 6) cost = 6;
    pet.happy  = clamp_add(pet.happy, 20 + 2 * pet.skill_lvl[SK_ACRO]);
    pet.energy = clamp_add(pet.energy, -cost);
    pet.hunger = clamp_add(pet.hunger, -4);
    pet.weight = clamp_add(pet.weight, -10);     /* exercise trims it down  */
    pet.muscle = clamp_add(pet.muscle, 9);       /* training builds muscle  */
    add_skill_xp(SK_ACRO, 9);
    add_xp(12);
    set_override(DOLPHIN_MOOD_PLAYING, 36);
    pet_save();
}

static void act_sleep_toggle(void)
{
    pet.sleeping = !pet.sleeping;
    override_frames = 0;
    pet_save();
}

static void act_wash(void)
{
    pet.hygiene = 100;
    pet.poop    = 0;                              /* scrub away the mess     */
    pet.happy   = clamp_add(pet.happy, 4 + pet.skill_lvl[SK_GROOM]);
    add_skill_xp(SK_GROOM, 7);
    add_xp(8);
    set_override(DOLPHIN_MOOD_HAPPY, 22);
    pet_save();
}

static void btn_event_cb(lv_event_t *e)
{
    if (pet.dead) return;
    void (*action)(void) = lv_event_get_user_data(e);
    action();
    base_mood = compute_base_mood();
    refresh_ui();
}

/* ---- info / skills panel ---------------------------------------------- */
static lv_obj_t *bar_skill[SK_COUNT];
static lv_obj_t *lbl_skill[SK_COUNT];
static lv_obj_t *lbl_pxp;

static void panel_update(void)
{
    if (!panel) return;
    char b[40];

    time_t n = (time_t)clock_now();
    struct tm tmv; localtime_r(&n, &tmv);
    strftime(b, sizeof(b), "%H:%M:%S", &tmv);
    lv_label_set_text(pnl_time, b);
    strftime(b, sizeof(b), "%a %d %b %Y", &tmv);
    lv_label_set_text(pnl_date, b);

    snprintf(b, sizeof(b), "%u / %u XP", (unsigned)pet.xp, (unsigned)level_need(pet.level));
    lv_label_set_text(lbl_pxp, b);

    for (int i = 0; i < SK_COUNT; i++) {
        snprintf(b, sizeof(b), "%s  Lv %u", SKILL_NAMES[i], pet.skill_lvl[i]);
        lv_label_set_text(lbl_skill[i], b);
        lv_bar_set_value(bar_skill[i],
            (int)(pet.skill_xp[i] * 100 / skill_need(pet.skill_lvl[i])), LV_ANIM_OFF);
    }
}

static void panel_close_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (panel) { lv_obj_del(panel); panel = NULL; }
}

static void time_adj_cb(lv_event_t *e)
{
    int delta = (int)(intptr_t)lv_event_get_user_data(e);
    clock_set(clock_now() + delta);
    pet.epoch = clock_now();
    pet_save();
    panel_update();
    refresh_ui();
}

static lv_obj_t *small_btn(lv_obj_t *parent, int x, int y, int w, int h,
                           const char *txt, uint32_t color,
                           lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_radius(b, 7, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0x0A2233), 0);
    lv_obj_center(l);
    return b;
}

static void open_panel_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (panel || pet.dead) return;
    panel = lv_obj_create(scr);
    lv_obj_set_size(panel, 172, 320);
    lv_obj_set_pos(panel, 0, 0);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x07294A), 0);
    lv_obj_set_style_bg_grad_color(panel, lv_color_hex(0x041829), 0);
    lv_obj_set_style_bg_grad_dir(panel, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    char b[40];
    lv_obj_t *t = lv_label_create(panel);
    snprintf(b, sizeof(b), "%s", pet.name);
    lv_label_set_text(t, b);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 8);

    lbl_pxp = lv_label_create(panel);
    lv_obj_set_style_text_font(lbl_pxp, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_pxp, lv_color_hex(0xCDEBFF), 0);
    lv_obj_align(lbl_pxp, LV_ALIGN_TOP_MID, 0, 36);

    /* ---- clock ---- */
    pnl_time = lv_label_create(panel);
    lv_obj_set_style_text_font(pnl_time, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(pnl_time, lv_color_hex(0x8FE0F2), 0);
    lv_obj_align(pnl_time, LV_ALIGN_TOP_MID, 0, 58);

    pnl_date = lv_label_create(panel);
    lv_obj_set_style_text_font(pnl_date, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(pnl_date, lv_color_hex(0xCDEBFF), 0);
    lv_obj_align(pnl_date, LV_ALIGN_TOP_MID, 0, 84);

    small_btn(panel, 10,  104, 34, 26, "H-", 0x8FE0F2, time_adj_cb, (void *)(intptr_t)(-3600));
    small_btn(panel, 48,  104, 34, 26, "H+", 0x8FE0F2, time_adj_cb, (void *)(intptr_t)(3600));
    small_btn(panel, 90,  104, 34, 26, "M-", 0xA6E3B6, time_adj_cb, (void *)(intptr_t)(-60));
    small_btn(panel, 128, 104, 34, 26, "M+", 0xA6E3B6, time_adj_cb, (void *)(intptr_t)(60));

    lv_obj_t *sh = lv_label_create(panel);
    lv_label_set_text(sh, "Skills");
    lv_obj_set_style_text_font(sh, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(sh, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(sh, 10, 140);

    for (int i = 0; i < SK_COUNT; i++) {
        int y = 160 + i * 26;
        lbl_skill[i] = lv_label_create(panel);
        lv_obj_set_style_text_font(lbl_skill[i], &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(lbl_skill[i], lv_color_hex(0xEAF6FF), 0);
        lv_obj_set_pos(lbl_skill[i], 10, y);

        bar_skill[i] = lv_bar_create(panel);
        lv_obj_set_size(bar_skill[i], 152, 6);
        lv_obj_set_pos(bar_skill[i], 10, y + 14);
        lv_bar_set_range(bar_skill[i], 0, 100);
        lv_obj_set_style_radius(bar_skill[i], 3, 0);
        lv_obj_set_style_bg_color(bar_skill[i], lv_color_hex(0x123A57), 0);
        lv_obj_set_style_radius(bar_skill[i], 3, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(bar_skill[i], lv_color_hex(0xFFC078), LV_PART_INDICATOR);
    }

    small_btn(panel, 46, 282, 80, 30, "Close", 0xFF8FB3, panel_close_cb, NULL);

    panel_update();
}

/* ---- timers ----------------------------------------------------------- */
static void anim_timer_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    frame++;
    if (override_frames > 0) override_frames--;
    dolphin_mood_t shown = override_frames > 0 ? override_mood : base_mood;
    dolphin_render(shown, frame);
}

static void logic_timer_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    if (pet.dead) return;
    decay_acc++;

    if (pet.sleeping) {
        int regen = 2 + pet.skill_lvl[SK_STAM] / 2;
        pet.energy = clamp_add(pet.energy, regen);
        if (decay_acc % 6 == 0)  pet.hunger  = clamp_add(pet.hunger, -1);
        if (decay_acc % 14 == 0) pet.hygiene = clamp_add(pet.hygiene, -1);
        if (decay_acc % 5 == 0)  { add_skill_xp(SK_STAM, 1); add_xp(1); }
        if (pet.energy >= 100) pet.sleeping = 0;
    } else {
        if (decay_acc % 6 == 0)  pet.hunger  = clamp_add(pet.hunger, -1);
        if (decay_acc % 8 == 0)  pet.energy  = clamp_add(pet.energy, -1);
        if (decay_acc % 10 == 0) pet.hygiene = clamp_add(pet.hygiene, -1);
        if (decay_acc % 7 == 0) {
            int d = -1;
            if (pet.hunger < 25 || pet.hygiene < 25 || pet.poop > 0) d -= 1;
            pet.happy = clamp_add(pet.happy, d);
        }
        if (decay_acc % 18 == 0) pet.weight = clamp_add(pet.weight, -1);
        if (decay_acc % 16 == 0) pet.muscle = clamp_add(pet.muscle, -1);  /* atrophy */
    }

    /* an uncleaned mess fouls the water faster */
    if (pet.poop > 0 && decay_acc % 4 == 0)
        pet.hygiene = clamp_add(pet.hygiene, -pet.poop);

    /* obesity strain: sustained high weight wears the dolphin down */
    if (pet.weight >= 90) {
        pet.strain = clamp_add(pet.strain, pet.weight >= 98 ? 3 : 1);
    } else if (pet.weight <= 78 && decay_acc % 2 == 0) {
        pet.strain = clamp_add(pet.strain, -1);
    }

    if (decay_acc % 60 == 0) pet.age_min++;
    if (flash_secs > 0) flash_secs--;

    if (pet.strain >= 100) {           /* died of obesity */
        pet.dead = 1;
        pet_save();
        show_death();
        return;
    }

    base_mood = compute_base_mood();
    refresh_ui();

    if (++save_acc >= 20) { save_acc = 0; pet_save(); }
}

/* ---- game ui ---------------------------------------------------------- */
static lv_obj_t *make_bar(lv_obj_t *parent, lv_coord_t y, const char *name, uint32_t color)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, name);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xEAF6FF), 0);
    lv_obj_set_pos(lbl, 6, y - 1);

    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_size(bar, 116, 8);
    lv_obj_set_pos(bar, 50, y);
    lv_bar_set_range(bar, 0, 100);
    lv_obj_set_style_radius(bar, 4, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x123A57), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_hex(color), LV_PART_INDICATOR);
    return bar;
}

static lv_obj_t *make_button(lv_obj_t *parent, lv_coord_t x, const char *txt,
                             uint32_t color, void (*action)(void))
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 39, 26);
    lv_obj_set_pos(btn, x, 290);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED, action);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x0A2233), 0);
    lv_obj_center(lbl);
    return lbl;
}

static void build_ui(void)
{
    scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0B3C66), 0);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(0x05223D), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scr, 0, 0);

    /* header: clock | name | level chip */
    lbl_clock = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_clock, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_clock, lv_color_hex(0x8FE0F2), 0);
    lv_obj_set_pos(lbl_clock, 6, 4);

    lbl_name = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_name, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_name, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(lbl_name, LV_ALIGN_TOP_MID, 0, 2);

    lv_obj_t *lvbtn = lv_btn_create(scr);
    lv_obj_set_size(lvbtn, 44, 20);
    lv_obj_set_pos(lvbtn, 124, 3);
    lv_obj_set_style_radius(lvbtn, 6, 0);
    lv_obj_set_style_bg_color(lvbtn, lv_color_hex(0xFFC078), 0);
    lv_obj_set_style_shadow_width(lvbtn, 0, 0);
    lv_obj_set_style_pad_all(lvbtn, 0, 0);
    lv_obj_add_event_cb(lvbtn, open_panel_cb, LV_EVENT_CLICKED, NULL);
    lbl_level = lv_label_create(lvbtn);
    lv_obj_set_style_text_font(lbl_level, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_level, lv_color_hex(0x0A2233), 0);
    lv_obj_center(lbl_level);

    /* xp bar */
    bar_xp = lv_bar_create(scr);
    lv_obj_set_size(bar_xp, 160, 6);
    lv_obj_set_pos(bar_xp, 6, 26);
    lv_bar_set_range(bar_xp, 0, 100);
    lv_obj_set_style_radius(bar_xp, 3, 0);
    lv_obj_set_style_bg_color(bar_xp, lv_color_hex(0x123A57), 0);
    lv_obj_set_style_radius(bar_xp, 3, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar_xp, lv_color_hex(0xFFD23F), LV_PART_INDICATOR);

    /* stat bars */
    bar_food  = make_bar(scr, 38, "Food", 0xFF9F40);
    bar_fun   = make_bar(scr, 50, "Fun",  0xFF5C8A);
    bar_rest  = make_bar(scr, 62, "Rest", 0x57D38B);
    bar_clean = make_bar(scr, 74, "Wash", 0x49C7E8);

    /* dolphin stage */
    dolphin_create(scr, 2, 90);

    /* status line */
    lbl_status = lv_label_create(scr);
    lv_obj_set_width(lbl_status, 168);
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(0xCDEBFF), 0);
    lv_obj_set_style_text_align(lbl_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_status, LV_ALIGN_TOP_MID, 0, 270);

    /* action buttons */
    make_button(scr, 3,   "Feed",  0xFFC078, act_feed);
    make_button(scr, 46,  "Play",  0xFF8FB3, act_play);
    btn_sleep_lbl =
    make_button(scr, 89,  "Sleep", 0xA6E3B6, act_sleep_toggle);
    make_button(scr, 132, "Wash",  0x8FE0F2, act_wash);
}

static void stop_game_timers(void)
{
    if (g_anim)  { lv_timer_del(g_anim);  g_anim  = NULL; }
    if (g_logic) { lv_timer_del(g_logic); g_logic = NULL; }
}

static void start_game(void)
{
    clock_set(pet.epoch);
    dolphin_set_dead(0);
    build_ui();
    base_mood = compute_base_mood();
    refresh_ui();
    g_anim  = lv_timer_create(anim_timer_cb, 80, NULL);
    g_logic = lv_timer_create(logic_timer_cb, 1000, NULL);
    ESP_LOGI(TAG, "game started for '%s'", pet.name);
}

/* ---- death screen ----------------------------------------------------- */
static void newpet_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    stop_game_timers();
    dolphin_set_dead(0);
    dolphin_forget();                 /* canvas is about to be deleted */
    if (panel) { lv_obj_del(panel); panel = NULL; }
    show_name_entry();
}

static void show_death(void)
{
    dolphin_set_dead(1);              /* float belly-up in the stage above */

    lv_obj_t *ov = lv_obj_create(scr);
    lv_obj_set_size(ov, 172, 152);
    lv_obj_set_pos(ov, 0, 168);
    lv_obj_set_style_radius(ov, 0, 0);
    lv_obj_set_style_bg_color(ov, lv_color_hex(0x1A0F08), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ov, 0, 0);
    lv_obj_set_style_pad_all(ov, 0, 0);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);

    char b[48];
    lv_obj_t *rip = lv_label_create(ov);
    lv_label_set_text(rip, "R.I.P.");
    lv_obj_set_style_text_font(rip, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(rip, lv_color_hex(0xE8E8E8), 0);
    lv_obj_align(rip, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *l1 = lv_label_create(ov);
    snprintf(b, sizeof(b), "%s died of obesity", pet.name);
    lv_label_set_text(l1, b);
    lv_obj_set_style_text_font(l1, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l1, lv_color_hex(0xFFC078), 0);
    lv_obj_align(l1, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t *l2 = lv_label_create(ov);
    snprintf(b, sizeof(b), "Lv %u  -  %lu days old", pet.level,
             (unsigned long)(pet.age_min / (60 * 24) + 1));
    lv_label_set_text(l2, b);
    lv_obj_set_style_text_font(l2, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(l2, lv_color_hex(0xCDEBFF), 0);
    lv_obj_align(l2, LV_ALIGN_TOP_MID, 0, 64);

    small_btn(ov, 36, 100, 100, 40, "New pet", 0x8FE0F2, newpet_cb, NULL);
    ESP_LOGI(TAG, "'%s' died of obesity at Lv%u", pet.name, pet.level);
}

/* ---- name entry ------------------------------------------------------- */
static void entry_refresh(void)
{
    lv_label_set_text(entry_preview, entry_buf[0] ? entry_buf : "_");
    char c[2] = { CHARSET[entry_idx], 0 };
    lv_label_set_text(entry_letter, c[0] == ' ' ? "[space]" : c);
}

static void entry_prev_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    int n = (int)strlen(CHARSET);
    entry_idx = (entry_idx - 1 + n) % n;
    entry_refresh();
}
static void entry_next_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    int n = (int)strlen(CHARSET);
    entry_idx = (entry_idx + 1) % n;
    entry_refresh();
}
static void entry_add_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    size_t len = strlen(entry_buf);
    if (len < NAME_MAX) {
        entry_buf[len] = CHARSET[entry_idx];
        entry_buf[len + 1] = 0;
        entry_refresh();
    }
}
static void entry_del_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    size_t len = strlen(entry_buf);
    if (len > 0) { entry_buf[len - 1] = 0; entry_refresh(); }
}
static void entry_ok_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    /* trim trailing spaces */
    for (int i = (int)strlen(entry_buf) - 1; i >= 0 && entry_buf[i] == ' '; i--) entry_buf[i] = 0;
    pet_reset(entry_buf);
    pet_save();
    if (entry_root) { lv_obj_del(entry_root); entry_root = NULL; }
    start_game();
}

static void show_name_entry(void)
{
    entry_buf[0] = 0;
    entry_idx = 0;

    scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0B3C66), 0);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(0x05223D), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scr, 0, 0);

    entry_root = lv_obj_create(scr);
    lv_obj_set_size(entry_root, 172, 320);
    lv_obj_set_pos(entry_root, 0, 0);
    lv_obj_set_style_bg_opa(entry_root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(entry_root, 0, 0);
    lv_obj_set_style_pad_all(entry_root, 0, 0);
    lv_obj_clear_flag(entry_root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(entry_root);
    lv_label_set_text(title, "Name your dolphin");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    entry_preview = lv_label_create(entry_root);
    lv_obj_set_style_text_font(entry_preview, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(entry_preview, lv_color_hex(0x8FE0F2), 0);
    lv_obj_align(entry_preview, LV_ALIGN_TOP_MID, 0, 52);

    entry_letter = lv_label_create(entry_root);
    lv_obj_set_style_text_font(entry_letter, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(entry_letter, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(entry_letter, LV_ALIGN_TOP_MID, 0, 104);

    small_btn(entry_root, 12,  98,  40, 40, "<",   0xA6E3B6, entry_prev_cb, NULL);
    small_btn(entry_root, 120, 98,  40, 40, ">",   0xA6E3B6, entry_next_cb, NULL);
    small_btn(entry_root, 20,  158, 60, 34, "Add", 0xFFC078, entry_add_cb,  NULL);
    small_btn(entry_root, 92,  158, 60, 34, "Del", 0xFF8FB3, entry_del_cb,  NULL);
    small_btn(entry_root, 40,  214, 92, 44, "OK",  0x8FE0F2, entry_ok_cb,   NULL);

    entry_refresh();
}

/* ---- entry point ------------------------------------------------------ */
void tamagotchi_start(void)
{
    if (pet_load()) {
        start_game();
    } else {
        show_name_entry();
    }
}
