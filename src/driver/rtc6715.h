#pragma once

#ifdef __cplusplus
extern "C" {
#endif
#include <stdbool.h>
typedef struct {
    void (*init)(bool power_on, bool audio_on);
    void (*set_ch)(int ch);
    int rssi;
} rtc6715_t;

extern void *thread_rtc6715_rssi(void *ptr);

#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
// One RSSI reading in mV, now. The thread above only reads it while the
// analog receiver is being watched.
int rtc6715_get_rssi();
// Start-up only: power the built-in receiver now so that the switch's
// rtc6715.init(1, ...) waits only for what is left of its 200 ms settling.
void rtc6715_power_early(void);
#endif

extern rtc6715_t rtc6715;
#ifdef __cplusplus
}
#endif
