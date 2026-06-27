#ifndef TAMAGOTCHI_H
#define TAMAGOTCHI_H

#ifdef __cplusplus
extern "C" {
#endif

/* Build the UI and start the game. Call with the LVGL lock held. */
void tamagotchi_start(void);

#ifdef __cplusplus
}
#endif

#endif /* TAMAGOTCHI_H */
