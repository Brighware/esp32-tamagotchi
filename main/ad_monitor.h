#ifndef __AD_MONITOR_H_USED__
#define __AD_MONITOR_H_USED__

#ifdef __cplusplus
extern "C" {
#endif

extern void init_ad_monitor(void);
extern void exec_ad_monitor(void);
extern int get_raw_ad_value(void);
extern int get_ad_voltage(void);

#ifdef __cplusplus
}
#endif

#endif /* WATCH_H */
