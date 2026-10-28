#include "gpadc.h"

#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <log/log.h>

#include "../core/common.hh"
#include "util/system.h"

void gpadc_init() {
    // open gpadc clock
    reg_write(0x030019ec, 0x00010001);

    // open adc channel 0
    reg_write(0x05070008, 0x00000001);
}

void gpadc_on(uint8_t is_on) {
    reg_write(0x05070004, is_on ? 0xffbd0000 : 0xffbc0000);
}

int gpdac0_get() {
    char buf[128];

#ifdef EMULATOR_BUILD
    return -1;
#endif

    sprintf(buf, "awr 0x05070080 > %s", ADC0_FILE);
    system(buf);
    usleep(10 * 1000); // 10ms

    FILE *file;
    file = fopen(ADC0_FILE, "r");
    if (file == NULL) {
        LOGI("%s open failed", ADC0_FILE);
        return 1;
    }

    uint32_t addr, reg;

    if (fscanf(file, "read 0x%x:0x%x", &addr, &reg) == 2) {
        fclose(file);
        if (addr == 0x05070080)
            return reg;
        else
            return -1;
    }

    fclose(file);
    return -1;
}
#else
void gpadc_init() {}
void gpadc_on(uint8_t is_on) {}
int gpdac0_get() {}
#endif
