/*
 * dolphin.c — a software-drawn, anatomically-styled bottlenose dolphin on an
 * LVGL canvas, with choreographed feed and play animations.
 *
 * The body is modelled as length "stations" (back / belly / two countershading
 * boundaries) and filled as three coloured triangle strips (dark dorsal cape,
 * medium flank, pale belly). A falcate dorsal fin, notched flukes, a swept
 * pectoral flipper, a melon highlight and the eye / mouth / blowhole sit on top.
 *
 * Every body part is drawn through a 2D transform (translate + rotate about the
 * body centre) so the whole animal can lean and arc. Two scripted sequences use
 * it: FEED (a fish swims in, the dolphin noses up, catches it with a chomp and
 * crumbs, then savours) and PLAY (a porpoising arc with a body tilt while it
 * nudges a bouncing beach ball, trailing bubbles and splash droplets).
 *
 * Only 3-point triangles are ever filled (LVGL's polygon fill corrupts memory
 * past ~16 vertices). All drawing is on the LVGL task, so no locking.
 */
#include "dolphin.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

#define STAGE_W 168
#define STAGE_H 176
#define PI 3.14159265f

#define HEAD_X 140
#define BODY_TOP 70

/* sequence lengths in animation frames (must be <= override frames set by the
 * game so the whole sequence gets to play) */
#define FEED_N 30
#define PLAY_N 34

static lv_color_t s_cbuf[STAGE_W * STAGE_H];
static lv_obj_t  *s_canvas = NULL;

/* action timeline */
static int      s_action = 0;       /* 0 none, 1 feed, 2 play */
static uint32_t s_action_f0 = 0;

/* body fatness + muscle + poop state (driven by the game) */
static int      s_fat_px = 0;
static int      s_musc_px = 0;
static int      s_poop = 0;
static int      s_poop_drop = 0;    /* frames remaining of the drop anim */
static int      s_dead = 0;

void dolphin_set_fat(int px)    { s_fat_px = px < 0 ? 0 : (px > 16 ? 16 : px); }
void dolphin_set_muscle(int px) { s_musc_px = px < 0 ? 0 : (px > 12 ? 12 : px); }
void dolphin_set_poop(int n)    { s_poop = n < 0 ? 0 : (n > 99 ? 99 : n); }
void dolphin_poop_drop(void)    { s_poop_drop = 16; }
void dolphin_set_dead(int d)    { s_dead = d ? 1 : 0; }
void dolphin_forget(void)       { s_canvas = NULL; }

/* ---- palette (cool grey bottlenose) ----------------------------------- */
static lv_color_t col_flank(void)   { return lv_color_hex(0x6F8A9B); }
static lv_color_t col_cape(void)    { return lv_color_hex(0x39505E); }
static lv_color_t col_belly(void)   { return lv_color_hex(0xDEE8ED); }
static lv_color_t col_melon(void)   { return lv_color_hex(0x9CB7C5); }
static lv_color_t col_fin_d(void)   { return lv_color_hex(0x2B3D48); }
static lv_color_t col_line(void)    { return lv_color_hex(0x1E2D36); }
static lv_color_t col_white(void)   { return lv_color_hex(0xFFFFFF); }
static lv_color_t col_dark(void)    { return lv_color_hex(0x10314F); }
static lv_color_t col_drop(void)    { return lv_color_hex(0xCFEFFA); }
static lv_color_t col_heart(void)   { return lv_color_hex(0xFF5C8A); }
static lv_color_t col_fish(void)    { return lv_color_hex(0xFF9F40); }
static lv_color_t col_spark(void)   { return lv_color_hex(0xFFF4B0); }
static lv_color_t col_grime(void)   { return lv_color_hex(0x8FA06B); }

/* linear blend between two packed RGB colours (f: 0 = a, 1 = b) */
static lv_color_t mix(uint32_t a, uint32_t b, float f)
{
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
    int br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
    return lv_color_make((uint8_t)(ar + (br - ar) * f),
                         (uint8_t)(ag + (bg - ag) * f),
                         (uint8_t)(ab + (bb - ab) * f));
}

/* ---- 2D transform applied by every drawing helper --------------------- */
static float xf_ox = 0, xf_oy = 0, xf_c = 1, xf_s = 0, xf_px = 84, xf_py = 86;
static float xf_fy = 1;              /* -1 = mirror vertically (belly-up)   */

static void xf_reset(void) { xf_ox = 0; xf_oy = 0; xf_c = 1; xf_s = 0; xf_fy = 1; }
static void xf_set(float ox, float oy, float deg, float px, float py)
{
    xf_ox = ox; xf_oy = oy;
    float r = deg * (PI / 180.0f);
    xf_c = cosf(r); xf_s = sinf(r);
    xf_px = px; xf_py = py;
    xf_fy = 1;
}
static void xf_pt(float x, float y, lv_coord_t *ox, lv_coord_t *oy)
{
    float dx = x - xf_px, dy = y - xf_py;
    *ox = (lv_coord_t)lroundf(xf_px + (dx * xf_c - dy * xf_s) + xf_ox);
    *oy = (lv_coord_t)lroundf(xf_py + xf_fy * (dx * xf_s + dy * xf_c) + xf_oy);
}

/* ---- low level helpers (all transform-aware) -------------------------- */
static void fill_circle(int cx, int cy, int r, lv_color_t c)
{
    lv_coord_t tx, ty;
    xf_pt(cx, cy, &tx, &ty);
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = c;
    d.bg_opa = LV_OPA_COVER;
    d.radius = LV_RADIUS_CIRCLE;
    lv_canvas_draw_rect(s_canvas, tx - r, ty - r, 2 * r, 2 * r, &d);
}

static void fill_tri(int x0, int y0, int x1, int y1, int x2, int y2, lv_color_t c)
{
    lv_point_t p[3];
    xf_pt(x0, y0, &p[0].x, &p[0].y);
    xf_pt(x1, y1, &p[1].x, &p[1].y);
    xf_pt(x2, y2, &p[2].x, &p[2].y);
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = c;
    d.bg_opa = LV_OPA_COVER;
    lv_canvas_draw_polygon(s_canvas, p, 3, &d);
}

static void draw_seg(int x0, int y0, int x1, int y1, int w, lv_color_t c)
{
    lv_point_t p[2];
    xf_pt(x0, y0, &p[0].x, &p[0].y);
    xf_pt(x1, y1, &p[1].x, &p[1].y);
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = c;
    d.width = w;
    d.round_start = 1;
    d.round_end = 1;
    lv_canvas_draw_line(s_canvas, p, 2, &d);
}

static void draw_polyline4(int x0, int y0, int x1, int y1, int x2, int y2,
                           int x3, int y3, int w, lv_color_t c)
{
    lv_point_t p[4];
    xf_pt(x0, y0, &p[0].x, &p[0].y);
    xf_pt(x1, y1, &p[1].x, &p[1].y);
    xf_pt(x2, y2, &p[2].x, &p[2].y);
    xf_pt(x3, y3, &p[3].x, &p[3].y);
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = c;
    d.width = w;
    d.round_start = 1;
    d.round_end = 1;
    lv_canvas_draw_line(s_canvas, p, 4, &d);
}

static void stroke_arc(int cx, int cy, int r, int a0, int a1, int w, lv_color_t c)
{
    lv_coord_t tx, ty;
    xf_pt(cx, cy, &tx, &ty);
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.color = c;
    d.width = w;
    d.rounded = 1;
    lv_canvas_draw_arc(s_canvas, tx, ty, r, a0, a1, &d);
}

static void draw_text(int x, int y, const char *txt, const lv_font_t *font, lv_color_t c)
{
    lv_coord_t tx, ty;
    xf_pt(x, y, &tx, &ty);
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.color = c;
    d.font = font;
    lv_canvas_draw_text(s_canvas, tx, ty, 40, &d, (char *)txt);
}

static void draw_heart(int cx, int cy, int r, lv_color_t c)
{
    fill_circle(cx - r / 2, cy - r / 3, r / 2 + 1, c);
    fill_circle(cx + r / 2, cy - r / 3, r / 2 + 1, c);
    fill_tri(cx - r, cy, cx + r, cy, cx, cy + r + 2, c);
}

static void draw_fish(int cx, int cy, lv_color_t c)
{
    fill_circle(cx, cy, 6, c);
    fill_tri(cx + 5, cy, cx + 14, cy - 6, cx + 14, cy + 6, c);  /* tail (points right) */
    fill_tri(cx - 1, cy - 6, cx + 3, cy - 1, cx - 4, cy - 1, c); /* dorsal */
    fill_circle(cx - 3, cy - 2, 1, col_dark());                  /* eye, faces left */
}

static void draw_sparkle(int cx, int cy, int r, lv_color_t c)
{
    fill_tri(cx, cy - r, cx - 2, cy, cx + 2, cy, c);
    fill_tri(cx, cy + r, cx - 2, cy, cx + 2, cy, c);
    fill_tri(cx - r, cy, cx, cy - 2, cx, cy + 2, c);
    fill_tri(cx + r, cy, cx, cy - 2, cx, cy + 2, c);
}

static void draw_ball(int cx, int cy, int r)
{
    static const uint32_t seg[3] = {0xFF6B6B, 0xFFD23F, 0x4FC3F7};
    fill_circle(cx, cy, r, col_white());
    for (int k = 0; k < 6; k += 2) {
        float a0 = k * 60 * (PI / 180.0f), a1 = (k + 1) * 60 * (PI / 180.0f);
        int x0 = cx + (int)lroundf((r - 1) * cosf(a0));
        int y0 = cy + (int)lroundf((r - 1) * sinf(a0));
        int x1 = cx + (int)lroundf((r - 1) * cosf(a1));
        int y1 = cy + (int)lroundf((r - 1) * sinf(a1));
        fill_tri(cx, cy, x0, y0, x1, y1, lv_color_hex(seg[(k / 2) % 3]));
    }
}

static void draw_poop(int cx, int cy, int s)
{
    lv_color_t br = lv_color_hex(0x6E4A2A), hi = lv_color_hex(0x8A5E36);
    fill_circle(cx, cy, s, br);
    fill_circle(cx - 1, cy - s, (s * 3) / 4, br);
    fill_circle(cx + 1, cy - (s * 3) / 2, s / 2 + 1, br);
    fill_circle(cx, cy - 2 * s, 1, br);                 /* curl tip */
    fill_circle(cx - s / 2, cy - 1, s / 3 + 1, hi);     /* highlight */
}

/* resting poop blobs on the sea floor, wrapping into rows as they pile up */
static void draw_poops(void)
{
    int shown = s_poop > 12 ? 12 : s_poop;
    for (int i = 0; i < shown; i++) {
        int col = i % 6, row = i / 6;
        draw_poop(18 + col * 26, STAGE_H - 15 - row * 13, 5);
    }
}

/* ---- dolphin body model (stations from tail stock to rostrum tip) ------ */
#define NST 13
static const int16_t SX[NST]    = { 25, 30, 40, 54, 68, 84, 100, 113, 123, 131, 140, 150, 154 };
static const int16_t YTOP[NST]  = { 84, 82, 79, 75, 71, 69,  70,  72,  74,  77,  83,  87,  88 };
static const int16_t YCAPE[NST] = { 86, 85, 85, 85, 84, 83,  83,  83,  82,  83,  86,  88,  88 };
static const int16_t YCNT[NST]  = { 87, 87, 89, 91, 91, 91,  90,  89,  87,  86,  87,  88,  88 };
static const int16_t YBOT[NST]  = { 88, 90, 95,100,104,105, 103,  99,  95,  92,  90,  89,  88 };
/* how much each station's belly bulges when the dolphin is fat (tenths) */
static const int16_t BULGE[NST] = {  0,  1,  3,  6,  9, 10,   9,   7,   4,   2,   1,   0,   0 };
/* how much each station's back lifts when the dolphin is muscular (tenths) */
static const int16_t MBACK[NST] = {  2,  4,  6,  8, 10, 10,   9,   7,   5,   3,   1,   0,   0 };

static void fill_strip(const int16_t *ytop, const int16_t *ybot, lv_color_t c)
{
    for (int i = 0; i < NST - 1; i++) {
        int x0 = SX[i], x1 = SX[i + 1];
        fill_tri(x0, ytop[i], x0, ybot[i], x1, ytop[i + 1], c);
        fill_tri(x0, ybot[i], x1, ybot[i + 1], x1, ytop[i + 1], c);
    }
}

/* Draw the whole dolphin body in its local coordinates; the active transform
 * places and tilts it. mouth_open / look let the sequences animate the face. */
static void draw_body(uint32_t frame, dolphin_mood_t mood, bool sleeping,
                      bool negative, bool mouth_open, int look)
{
    int tailf = (int)(6.0f * sinf(frame * 0.5f + 1.0f));
    int pecf  = (int)(3.0f * sinf(frame * 0.42f));
    if (s_action == 2) { tailf = (int)(8.0f * sinf(frame * 0.9f)); }
    if (s_dead) { tailf = 0; pecf = 0; }

    /* dead dolphins go grey */
    lv_color_t cF  = s_dead ? lv_color_hex(0x8A95A0) : col_flank();
    lv_color_t cC  = s_dead ? lv_color_hex(0x59636D) : col_cape();
    lv_color_t cB  = s_dead ? lv_color_hex(0xD6DBDF) : col_belly();
    lv_color_t cFn = s_dead ? lv_color_hex(0x49525B) : col_fin_d();
    lv_color_t cM  = s_dead ? lv_color_hex(0x9AA4AD) : col_melon();

    /* tail flukes */
    fill_tri(29, 85, 5, 76 + tailf, 20, 88, cFn);
    fill_tri(29, 91, 5, 100 + tailf, 20, 88, cFn);

    /* body: belly bulges down with fat, back lifts up with muscle */
    int16_t ytop[NST], ycape[NST], ybot[NST], ycnt[NST];
    for (int i = 0; i < NST; i++) {
        int dn = s_fat_px * BULGE[i] / 10;
        int up = s_musc_px * MBACK[i] / 10;
        ytop[i]  = YTOP[i]  - up;
        ycape[i] = YCAPE[i] - up;
        ybot[i]  = YBOT[i]  + dn;
        ycnt[i]  = YCNT[i]  + (dn * 2) / 5;
    }
    fill_strip(ytop, ybot, cF);
    fill_strip(ytop, ycape, cC);
    fill_strip(ycnt, ybot, cB);

    /* a flexed-muscle crease along the flank when well-built */
    if (s_musc_px >= 5 && !s_dead) {
        draw_polyline4(58, 86, 76, 84, 96, 85, 112, 89, 1, lv_color_hex(0x2F4654));
    }

    /* melon sheen */
    fill_tri(130, 77, 119, 74, 124, 78, cM);

    /* falcate dorsal fin — taller and broader with muscle */
    int dup = s_musc_px * 7 / 10;
    int dgrow = s_musc_px / 4;
    fill_tri(93, 70 - dup, 76, 55 - dup - dgrow, 83, 70 - dup, cC);
    fill_tri(93, 70 - dup, 76, 55 - dup - dgrow, 86, 63 - dup, cFn);

    /* pectoral flipper — a touch larger with muscle */
    int pm = s_musc_px / 4;
    fill_tri(117, 89, 111, 93, 99, 108 + pecf + pm, cFn);
    fill_tri(117, 89, 99, 108 + pecf + pm, 106 + pm / 2, 98, cFn);

    /* blowhole */
    fill_circle(119, 72, 2, col_line());

    /* eye (an X when dead) */
    int ex = 132, ey = 87;
    if (s_dead) {
        draw_seg(ex - 3, ey - 3, ex + 3, ey + 3, 2, col_line());
        draw_seg(ex + 3, ey - 3, ex - 3, ey + 3, 2, col_line());
    } else if (sleeping) {
        stroke_arc(ex, ey + 1, 4, 20, 160, 2, col_line());
    } else {
        bool blink = ((frame % 45) < 2);
        if (blink) {
            stroke_arc(ex, ey, 4, 20, 160, 2, col_line());
        } else {
            int r = negative ? 2 : 3;
            fill_circle(ex, ey, r + 1, col_white());
            fill_circle(ex + look, ey, r, col_line());
            fill_circle(ex + look + 1, ey - 1, 1, col_white());
        }
    }

    /* mouth */
    if (mouth_open) {
        fill_circle(150, 90, 4, col_line());
        draw_polyline4(153, 88, 147, 86, 141, 87, 134, 86, 2, col_line());
    } else {
        int corner = negative ? 92 : 86;
        draw_polyline4(153, 89, 146, 90, 139, 90, 132, corner, 2, col_line());
    }

    if (mood == DOLPHIN_MOOD_SICK) {
        fill_circle(122, 84, 4, lv_color_hex(0x9CC7A0));
        fill_circle(96, 80, 3, lv_color_hex(0x9CC7A0));
    }
}

/* ---- public ----------------------------------------------------------- */
lv_obj_t *dolphin_create(lv_obj_t *parent, lv_coord_t x, lv_coord_t y)
{
    s_canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(s_canvas, s_cbuf, STAGE_W, STAGE_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(s_canvas, x, y);
    lv_obj_set_style_radius(s_canvas, 12, 0);
    lv_obj_set_style_clip_corner(s_canvas, true, 0);
    dolphin_render(DOLPHIN_MOOD_NORMAL, 0);
    return s_canvas;
}

void dolphin_render(dolphin_mood_t mood, uint32_t frame)
{
    if (!s_canvas) return;

    const float t = (float)frame * 0.4f;
    const bool sleeping = (mood == DOLPHIN_MOOD_SLEEPING);
    const bool negative = (mood == DOLPHIN_MOOD_SAD || mood == DOLPHIN_MOOD_SICK ||
                           mood == DOLPHIN_MOOD_HUNGRY || mood == DOLPHIN_MOOD_DIRTY);

    /* track the active scripted action and its elapsed frames */
    int act = (mood == DOLPHIN_MOOD_EATING) ? 1 : (mood == DOLPHIN_MOOD_PLAYING) ? 2 : 0;
    if (act != 0 && act != s_action) { s_action = act; s_action_f0 = frame; }
    if (act == 0) s_action = 0;
    int ap = (int)(frame - s_action_f0);

    /* ---- background (identity transform) ------------------------------ */
    /* uncleaned poop fouls the water: clear -> brown (f<.5) -> near black */
    float f = s_poop / 12.0f;
    if (f > 1) f = 1;
    lv_color_t wt, wb, sand, bub;
    if (f < 0.5f) {
        float g = f / 0.5f;
        wt   = mix(0x7FCBEE, 0x7A6630, g);
        wb   = mix(0x3E97CC, 0x4E3F1C, g);
        sand = mix(0xE9D9A6, 0x8A7A48, g);
        bub  = mix(0xCFEFFA, 0xB0A06A, g);
    } else {
        float g = (f - 0.5f) / 0.5f;
        wt   = mix(0x7A6630, 0x141008, g);
        wb   = mix(0x4E3F1C, 0x0C0A06, g);
        sand = mix(0x8A7A48, 0x18140C, g);
        bub  = mix(0xB0A06A, 0x2A2418, g);
    }

    xf_reset();
    lv_canvas_fill_bg(s_canvas, wt, LV_OPA_COVER);
    {
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = wb;
        d.bg_opa = LV_OPA_COVER;
        lv_canvas_draw_rect(s_canvas, 0, STAGE_H - 46, STAGE_W, 46, &d);
        d.bg_color = sand;
        lv_canvas_draw_rect(s_canvas, 0, STAGE_H - 12, STAGE_W, 12, &d);
    }
    for (int i = 0; i < 5; i++) {
        int bx = 14 + i * 30 + (int)(6 * sinf(t * 0.6f + i));
        int by = STAGE_H - 16 - ((frame * 2 + i * 30) % (STAGE_H - 10));
        fill_circle(bx, by, 2 + (i % 2), bub);
    }

    /* ---- dead: belly-up, grey, gently bobbing at the surface ----------- */
    if (s_dead) {
        float bobd = 2.5f * sinf(t * 0.35f);
        xf_set(0, -40 + bobd, 0, 84, 86);
        xf_fy = -1;
        draw_body(frame, mood, false, false, false, 0);
        xf_reset();
        draw_poops();
        lv_obj_invalidate(s_canvas);
        return;
    }

    /* ---- choreograph body placement ----------------------------------- */
    float bob = sleeping ? (2.0f * sinf(t * 0.5f)) : (4.0f * sinf(t));
    float ox = 0, oy = bob, ang = 0;
    bool mouth_open = (mood == DOLPHIN_MOOD_EATING);
    int look = (mood == DOLPHIN_MOOD_PLAYING) ? (int)lroundf(sinf(t)) : 0;

    float fu = 0;          /* feed sub-progress      */
    float pu = 0;          /* play sub-progress      */
    if (act == 1) {
        fu = ap / (float)FEED_N; if (fu > 1) fu = 1;
        float app = fu < 0.45f ? fu / 0.45f : 1.0f;
        float lean = sinf(app * PI * 0.5f);
        if (fu < 0.55f) {
            oy = bob - 9.0f * lean;        /* rise toward the fish     */
            ox = 5.0f * lean;
            ang = -13.0f * lean;           /* nose up                  */
            look = 1;                      /* eye tracks the fish      */
            mouth_open = (fu > 0.18f);     /* open just before catch   */
        } else {
            float sv = (fu - 0.55f) / 0.45f;
            oy = bob + 4.0f * sinf(sv * PI * 4) * (1 - sv);   /* happy wiggle */
            mouth_open = false;
        }
    } else if (act == 2) {
        pu = ap / (float)PLAY_N; if (pu > 1) pu = 1;
        oy = -34.0f * sinf(PI * pu) + 3.0f * sinf(pu * PI * 6);  /* arc + wiggle */
        ox = 9.0f * sinf(PI * pu);
        ang = -24.0f * cosf(PI * pu);      /* nose up rising, down falling */
    }
    int iox = (int)lroundf(ox), ioy = (int)lroundf(oy);

    /* ---- draw the body through the transform -------------------------- */
    xf_set(ox, oy, ang, 84, 86);
    draw_body(frame, mood, sleeping, negative, mouth_open, look);
    xf_reset();

    /* ---- scripted-action effects (world space) ------------------------ */
    if (act == 1) {
        if (fu < 0.5f) {
            /* fish swims in from the upper right toward the mouth */
            float e = fu / 0.5f;
            float ease = e * e * (3 - 2 * e);
            int fx = (int)lroundf(170 - (170 - (150 + iox)) * ease);
            int fy = (int)lroundf(14 - (14 - (88 + ioy)) * ease) + (int)(2 * sinf(fu * 18));
            draw_fish(fx, fy, col_fish());
        } else if (fu < 0.62f) {
            /* chomp! crumbs burst from the mouth */
            for (int i = 0; i < 5; i++) {
                float a = i * 1.3f - 0.6f;
                int rad = 4 + (int)((fu - 0.5f) * 90);
                int cx = 150 + iox + (int)(rad * cosf(a));
                int cy = 88 + ioy + (int)(rad * sinf(a));
                fill_circle(cx, cy, 2, col_fish());
            }
        } else {
            /* savour: a couple of hearts drift up */
            for (int i = 0; i < 2; i++) {
                int hx = 120 + i * 22;
                int hy = 64 + ioy - (int)((ap * 2 + i * 16) % 34);
                draw_heart(hx, hy, 4, col_heart());
            }
        }
    } else if (act == 2) {
        /* bouncing beach ball riding above the dolphin */
        int bx = 116 + (int)lroundf(24 * pu);
        int bbounce = (int)lroundf(24 * fabsf(sinf(pu * PI * 2.0f)) * (1.0f - 0.3f * pu));
        int by = 44 + ioy - bbounce;
        draw_ball(bx, by, 7);
        /* bubble trail along the body path */
        for (int i = 0; i < 4; i++) {
            int tx = 70 + iox + (int)(8 * sinf(pu * PI * 3 + i));
            int ty = 96 + ioy + i * 6 - (int)((ap * 3 + i * 9) % 24);
            fill_circle(tx, ty, 2, col_drop());
        }
        /* splash droplets near the top of the arc */
        if (pu > 0.3f && pu < 0.7f) {
            for (int i = 0; i < 4; i++) {
                float a = i * 1.4f;
                int sx = 84 + iox + (int)(14 * cosf(a));
                int sy = 52 + ioy + (int)(8 * sinf(a)) - (int)((ap * 2) % 8);
                fill_circle(sx, sy, 1 + (i & 1), col_drop());
            }
        }
    }

    /* ---- ambient mood effects ----------------------------------------- */
    switch (mood) {
        case DOLPHIN_MOOD_SLEEPING: {
            int zy = BODY_TOP - 6 + ioy;
            draw_text(HEAD_X - 2, zy - (int)(t) % 18, "z", &lv_font_montserrat_12, col_white());
            draw_text(HEAD_X + 8, zy - 12 - (int)(t * 1.2f) % 22, "Z", &lv_font_montserrat_16, col_white());
            draw_text(HEAD_X + 22, zy - 24 - (int)(t * 1.4f) % 26, "Z", &lv_font_montserrat_20, col_white());
            break;
        }
        case DOLPHIN_MOOD_HAPPY: {
            for (int i = 0; i < 3; i++) {
                int hx = 56 + i * 34;
                int hy = BODY_TOP - 6 - ((frame * 2 + i * 22) % 42);
                draw_heart(hx, hy, 5, col_heart());
            }
            if ((frame % 24) < 12) {
                draw_sparkle(108, 60, 4, col_spark());
                draw_sparkle(40, 96, 3, col_spark());
            }
            break;
        }
        case DOLPHIN_MOOD_DIRTY: {
            fill_circle(62, 80, 4, col_grime());
            fill_circle(95, 82, 3, col_grime());
            fill_circle(44, 84, 3, col_grime());
            break;
        }
        case DOLPHIN_MOOD_HUNGRY: {
            draw_text(HEAD_X + 4, BODY_TOP - 16, "!", &lv_font_montserrat_20, lv_color_hex(0xFFD23F));
            break;
        }
        default:
            break;
    }

    /* ---- poop on the sea floor + drop animation ----------------------- */
    draw_poops();
    if (s_poop_drop > 0) {
        float dp = 1.0f - (s_poop_drop / 16.0f);          /* 0..1 fall */
        int px = 34 + (int)(3 * sinf(dp * 10));
        int py = 112 + (int)((STAGE_H - 15 - 112) * dp);
        draw_poop(px, py, 4);
        s_poop_drop--;
    }

    lv_obj_invalidate(s_canvas);
}
