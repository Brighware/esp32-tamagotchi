#ifndef DOLPHIN_H
#define DOLPHIN_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DOLPHIN_MOOD_HAPPY = 0,
    DOLPHIN_MOOD_NORMAL,
    DOLPHIN_MOOD_SAD,
    DOLPHIN_MOOD_HUNGRY,
    DOLPHIN_MOOD_DIRTY,
    DOLPHIN_MOOD_SLEEPING,
    DOLPHIN_MOOD_SICK,
    DOLPHIN_MOOD_EATING,
    DOLPHIN_MOOD_PLAYING,
} dolphin_mood_t;

/* Create the underwater stage + dolphin canvas as a child of `parent`. */
lv_obj_t *dolphin_create(lv_obj_t *parent, lv_coord_t x, lv_coord_t y);

/* Redraw the scene for the given mood and animation frame counter. */
void dolphin_render(dolphin_mood_t mood, uint32_t frame);

/* Belly fatness in pixels of bulge (0 = slim). */
void dolphin_set_fat(int px);

/* Muscle/"ripped" build in pixels of back+shoulder bulge (0 = unbuilt). */
void dolphin_set_muscle(int px);

/* Number of poops resting on the sea floor (0 = none, unbounded). */
void dolphin_set_poop(int count);

/* Trigger a one-shot poop-drop animation. */
void dolphin_poop_drop(void);

/* When set, the dolphin is drawn dead (belly-up, grey, X eye). */
void dolphin_set_dead(int dead);

/* Forget the canvas pointer (call before the canvas object is deleted). */
void dolphin_forget(void);

#ifdef __cplusplus
}
#endif

#endif /* DOLPHIN_H */
