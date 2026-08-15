#ifndef WATCH_H
#define WATCH_H

#ifdef __cplusplus
extern "C" {
#endif
#include <stdbool.h>
/* Start the watch shell: analog watchface home + long-press app drawer.
 * Call with the LVGL lock held. */
void watch_start(void);

/* Return to the watchface (used by apps when they exit). */
void watch_go_home(void);

extern bool get_screen_timeout_flag(void);

#ifdef __cplusplus
}
#endif

#endif /* WATCH_H */
