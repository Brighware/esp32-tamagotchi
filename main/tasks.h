#ifndef __TASKS_H_USED__
#define __TASKS_H_USED__

#ifdef __cplusplus
extern "C" {
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern void init_tasks(void);
extern void create_new_task(TaskHandle_t handle, char *name, void *pvParameters);


#ifdef __cplusplus
}
#endif

#endif /* __TASKS_H_USED__ */
