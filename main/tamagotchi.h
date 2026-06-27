#ifndef TAMAGOTCHI_H
#define TAMAGOTCHI_H

#ifdef __cplusplus
extern "C" {
#endif

/* Launch the TamaWatchy app: builds its UI and starts its timers on the active
 * screen (which the caller has cleaned). Call with the LVGL lock held. */
void tamagotchi_open(void);

/* Tear the app down: stop timers, release the dolphin canvas, save the pet.
 * The caller is responsible for clearing the screen afterwards. */
void tamagotchi_close(void);

#ifdef __cplusplus
}
#endif

#endif /* TAMAGOTCHI_H */
