#include "time.h"

#include <time.h>

// Monotonic, not wall-clock: rtc_init() sets the system clock a fraction of a
// second into start-up, jumping it from 1970 to the present. Measured with
// gettimeofday() that made the boot total wrap to four billion milliseconds,
// and any interval spanning a later RTC or NTP adjustment would be off too.
static uint64_t monotonic_ms(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

uint32_t time_ms() {
    const uint64_t now_ms = monotonic_ms();

    static uint64_t start_ms = 0;
    if (start_ms == 0) {
        start_ms = now_ms;
    }

    return now_ms - start_ms;
}

uint32_t time_s() {
    const uint64_t now_s = monotonic_ms() / 1000;

    static uint64_t start_s = 0;
    if (start_s == 0) {
        start_s = now_s;
    }

    return now_s - start_s;
}
