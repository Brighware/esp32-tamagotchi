/*
 * clock.c — a tiny software real-time clock shared by the watchface and the
 * TamaWatchy app. The board has no RTC chip, so we run off the ESP32 system
 * clock and persist the time to NVS (namespace "watch") so it resumes on reboot.
 */
#include "clock.h"

#include <time.h>
#include <sys/time.h>

#include "esp_err.h"
#include "nvs.h"

#define NVS_NS  "watch"
#define NVS_KEY "epoch"

static int64_t default_epoch(void)
{
    struct tm t0 = {0};
    t0.tm_year = 2026 - 1900; t0.tm_mon = 5; t0.tm_mday = 27;
    t0.tm_hour = 12; t0.tm_min = 0; t0.tm_isdst = 0;
    return (int64_t)mktime(&t0);
}

static void set_system(int64_t epoch)
{
    struct timeval tv = { .tv_sec = (time_t)epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);
}

void clock_boot_init(void)
{
    int64_t e = default_epoch();
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        int64_t v;
        if (nvs_get_i64(h, NVS_KEY, &v) == ESP_OK && v > 0) e = v;
        nvs_close(h);
    }
    set_system(e);
}

int64_t clock_now(void)
{
    time_t n;
    time(&n);
    return (int64_t)n;
}

void clock_persist(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i64(h, NVS_KEY, clock_now());
        nvs_commit(h);
        nvs_close(h);
    }
}

void clock_adjust(int seconds)
{
    set_system(clock_now() + seconds);
    clock_persist();
}
