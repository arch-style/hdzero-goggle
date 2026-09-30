#include "hardware.h"

#if HDZGOGGLE2

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <log/log.h>

#include "../core/app_state.h"
#include "../core/common.hh"
#include "../core/osd.h"
#include "../core/settings.h"
#include "../ui/page_common.h"
#include "../util/system.h"
#include "beep.h"
#include "defines.h"
#include "dm5680.h"
#include "dm6302.h"
#include "dvr.h"
#include "hardware.h"
#include "i2c.h"
#include "it66021.h"
#include "it66121.h"
#include "msp_displayport.h"
#include "screen.h"
#include "tp2825.h"
#include "uart.h"
#include "ui/ui_porting.h"
#include "util/system.h"

/////////////////////////////////////////////////////////////////////////
// global
hw_status_t g_hw_stat;
int fhd_req = 0;
// local
pthread_mutex_t hardware_mutex;

uint32_t vclk_phase_default[VIDEO_SOURCE_NUM] = {
    // 0x??,  0x8e,  0x??, 0x??
    0x0000000e, // VIDEO_SOURCE_VERSION
    0x00200000, // VIDEO_SOURCE_MENU_UI
    0x00010000, // VIDEO_SOURCE_HDZERO_IN_720P60_50
    0x00200000, // VIDEO_SOURCE_HDZERO_IN_720P90
    0x00000000, // VIDEO_SOURCE_HDZERO_IN_1080P30
    0x00010000, // VIDEO_SOURCE_AV_IN
    0x00000000, // VIDEO_SOURCE_HDMI_IN_1080P50
    0x00000000, // VIDEO_SOURCE_HDMI_IN_1080P60
    0x00000000, // VIDEO_SOURCE_HDMI_IN_1080POTHER
    0x00000000, // VIDEO_SOURCE_HDMI_IN_720P50
    0x00000000, // VIDEO_SOURCE_HDMI_IN_720P60
    0x00200000, // VIDEO_SOURCE_HDMI_IN_720P100
    0x00000000, // VIDEO_SOURCE_TP2825_EX
};

/*
bit[0]   hdmi in
bit[1]   analog in
bit[2]   osd
bit[3]   dvr
bit[5:4] hdmi out
bit[6]   oled
*/
uint32_t pclk_phase_default[VIDEO_SOURCE_NUM] = {
    0x00000004,
    0x00000000, // VIDEO_SOURCE_MENU_UI
    0x00000000, // VIDEO_SOURCE_HDZERO_IN_720P60_50
    0x00000000, // VIDEO_SOURCE_HDZERO_IN_720P90
    0x00000000, // VIDEO_SOURCE_HDZERO_IN_1080P30
    0x00000000, // VIDEO_SOURCE_AV_IN
    0x00000001, // VIDEO_SOURCE_HDMI_IN_1080P50
    0x00000001, // VIDEO_SOURCE_HDMI_IN_1080P60
    0x00000001, // VIDEO_SOURCE_HDMI_IN_1080POTHER
    0x00000000, // VIDEO_SOURCE_HDMI_IN_720P50
    0x00000000, // VIDEO_SOURCE_HDMI_IN_720P60
    0x00000001, // VIDEO_SOURCE_HDMI_IN_720P100
    0x00000000, // VIDEO_SOURCE_TP2825_EX, DO NOT USE
};

uint32_t vclk_phase_load[VIDEO_SOURCE_NUM];
uint32_t vclk_phase[VIDEO_SOURCE_NUM];
uint32_t pclk_phase_load[VIDEO_SOURCE_NUM];
uint32_t pclk_phase[VIDEO_SOURCE_NUM];

uint8_t vclk_phase_read_file(char *file_path) {
    FILE *file;
    char line[256];
    char type_str[128];
    char value_str[16];
    char *endptr;
    uint32_t value;
    uint8_t i = 0;

    LOGI("vclk_phase_read_file: %s", file_path);
    for (i = 0; i < VIDEO_SOURCE_NUM; i++) {
        vclk_phase_load[i] = 0xffffffff;
    }

    file = fopen(file_path, "r");
    if (file == NULL) {
        LOGI("%s open failed", file_path);
        return 1;
    } else {
        while (fgets(line, sizeof(line), file)) {
            sscanf(line, "%s %s", type_str, value_str);
            value = strtoul(value_str, &endptr, 0);
            if (*endptr != '\0') {
                LOGI("vclk parse error: %s, %s", type_str, value_str);
                break;
            }
            LOGI("%s    0x%08x", type_str, value);

            if (strcmp(type_str, "VIDEO_SOURCE_VERSION") == 0)
                vclk_phase_load[VIDEO_SOURCE_VERSION] = value;
            else if (strcmp(type_str, "VIDEO_SOURCE_MENU_UI") == 0)
                vclk_phase_load[VIDEO_SOURCE_MENU_UI] = value;
            else if (strcmp(type_str, "VIDEO_SOURCE_HDZERO_IN_720P60_50") == 0)
                vclk_phase_load[VIDEO_SOURCE_HDZERO_IN_720P60_50] = value;
            else if (strcmp(type_str, "VIDEO_SOURCE_HDZERO_IN_720P90") == 0)
                vclk_phase_load[VIDEO_SOURCE_HDZERO_IN_720P90] = value;
            else if (strcmp(type_str, "VIDEO_SOURCE_HDZERO_IN_1080P30") == 0)
                vclk_phase_load[VIDEO_SOURCE_HDZERO_IN_1080P30] = value;
            else if (strcmp(type_str, "VIDEO_SOURCE_AV_IN") == 0)
                vclk_phase_load[VIDEO_SOURCE_AV_IN] = value;
            else if (strcmp(type_str, "VIDEO_SOURCE_HDMI_IN_1080P50") == 0)
                vclk_phase_load[VIDEO_SOURCE_HDMI_IN_1080P50] = value;
            else if (strcmp(type_str, "VIDEO_SOURCE_HDMI_IN_1080P60") == 0)
                vclk_phase_load[VIDEO_SOURCE_HDMI_IN_1080P60] = value;
            else if (strcmp(type_str, "VIDEO_SOURCE_HDMI_IN_1080POTHER") == 0)
                vclk_phase_load[VIDEO_SOURCE_HDMI_IN_1080POTHER] = value;
            else if (strcmp(type_str, "VIDEO_SOURCE_HDMI_IN_720P50") == 0)
                vclk_phase_load[VIDEO_SOURCE_HDMI_IN_720P50] = value;
            else if (strcmp(type_str, "VIDEO_SOURCE_HDMI_IN_720P60") == 0)
                vclk_phase_load[VIDEO_SOURCE_HDMI_IN_720P60] = value;
            else if (strcmp(type_str, "VIDEO_SOURCE_HDMI_IN_720P100") == 0)
                vclk_phase_load[VIDEO_SOURCE_HDMI_IN_720P100] = value;
            else if (strcmp(type_str, "VIDEO_SOURCE_TP2825_EX") == 0)
                vclk_phase_load[VIDEO_SOURCE_TP2825_EX] = value;
        }
    }

    fclose(file);
    return 0;
}

uint8_t pclk_phase_read_file(char *file_path) {
    FILE *file;
    char line[256];
    char type_str[128];
    char value_str[16];
    char *endptr;
    uint32_t value;
    uint8_t i = 0;

    LOGI("pclk_phase_read_file: %s", file_path);
    for (i = 0; i < VIDEO_SOURCE_NUM; i++) {
        pclk_phase_load[i] = 0xffffffff;
    }

    file = fopen(file_path, "r");
    if (file == NULL) {
        LOGI("%s open failed", file_path);
        return 1;
    } else {
        while (fgets(line, sizeof(line), file)) {
            sscanf(line, "%s %s", type_str, value_str);
            value = strtoul(value_str, &endptr, 0);
            if (*endptr != '\0') {
                LOGI("pclk parse error: %s, %s", type_str, value_str);
                break;
            }
            LOGI("%s    0x%08x", type_str, value);

            if (strcmp(type_str, "VIDEO_SOURCE_VERSION") == 0)
                pclk_phase_load[VIDEO_SOURCE_VERSION] = value;
            else if (strcmp(type_str, "VIDEO_SOURCE_MENU_UI") == 0)
                pclk_phase_load[VIDEO_SOURCE_MENU_UI] = value;
            else if (strcmp(type_str, "VIDEO_SOURCE_HDZERO_IN_720P60_50") == 0)
                pclk_phase_load[VIDEO_SOURCE_HDZERO_IN_720P60_50] = value;
            else if (strcmp(type_str, "VIDEO_SOURCE_HDZERO_IN_720P90") == 0)
                pclk_phase_load[VIDEO_SOURCE_HDZERO_IN_720P90] = value;
            else if (strcmp(type_str, "VIDEO_SOURCE_HDZERO_IN_1080P30") == 0)
                pclk_phase_load[VIDEO_SOURCE_HDZERO_IN_1080P30] = value;
            else if (strcmp(type_str, "VIDEO_SOURCE_AV_IN") == 0)
                pclk_phase_load[VIDEO_SOURCE_AV_IN] = value;
            else if (strcmp(type_str, "VIDEO_SOURCE_HDMI_IN_1080P50") == 0)
                pclk_phase_load[VIDEO_SOURCE_HDMI_IN_1080P50] = value;
            else if (strcmp(type_str, "VIDEO_SOURCE_HDMI_IN_1080P60") == 0)
                pclk_phase_load[VIDEO_SOURCE_HDMI_IN_1080P60] = value;
            else if (strcmp(type_str, "VIDEO_SOURCE_HDMI_IN_1080POTHER") == 0)
                pclk_phase_load[VIDEO_SOURCE_HDMI_IN_1080POTHER] = value;
            else if (strcmp(type_str, "VIDEO_SOURCE_HDMI_IN_720P50") == 0)
                pclk_phase_load[VIDEO_SOURCE_HDMI_IN_720P50] = value;
            else if (strcmp(type_str, "VIDEO_SOURCE_HDMI_IN_720P60") == 0)
                pclk_phase_load[VIDEO_SOURCE_HDMI_IN_720P60] = value;
            else if (strcmp(type_str, "VIDEO_SOURCE_HDMI_IN_720P100") == 0)
                pclk_phase_load[VIDEO_SOURCE_HDMI_IN_720P100] = value;
            else if (strcmp(type_str, "VIDEO_SOURCE_TP2825_EX") == 0)
                pclk_phase_load[VIDEO_SOURCE_TP2825_EX] = value;
        }
    }

    fclose(file);
    return 0;
}

uint8_t vclk_phase_inv_read_file(char *file_path) {
    FILE *file;
    char line[256];
    char type_str[128];
    char value_str[16];
    char *endptr;
    uint8_t mode = 0;
    uint32_t value;
    uint8_t i = 0;

    LOGI("vclk_phase_inv_read_file: %s", file_path);

    file = fopen(file_path, "r");
    if (file == NULL) {
        LOGI("%s open failed", file_path);
        return 1;
    } else {
        while (fgets(line, sizeof(line), file)) {
            sscanf(line, "%s %s", type_str, value_str);
            value = strtoul(value_str, &endptr, 0);
            if (*endptr != '\0') {
                LOGI("vclk parse inv error: %s, %s", type_str, value_str);
                break;
            }

            if (strcmp(type_str, "MODE1") == 0 || strcmp(type_str, "mode1") == 0)
                mode = 1;
            else if (strcmp(type_str, "MODE2") == 0 || strcmp(type_str, "mode2") == 0)
                mode = 2;
            else if (strcmp(type_str, "MODE3") == 0 || strcmp(type_str, "mode3") == 0)
                mode = 3;
            else if (strcmp(type_str, "MODE4") == 0 || strcmp(type_str, "mode4") == 0)
                mode = 4;
            else if (strcmp(type_str, "MODE5") == 0 || strcmp(type_str, "mode5") == 0)
                mode = 5;
            else if (strcmp(type_str, "MODE6") == 0 || strcmp(type_str, "mode6") == 0)
                mode = 6;
            else if (strcmp(type_str, "MODE7") == 0 || strcmp(type_str, "mode7") == 0)
                mode = 7;
            else if (strcmp(type_str, "MODE8") == 0 || strcmp(type_str, "mode8") == 0)
                mode = 8;
            else if (strcmp(type_str, "MODE9") == 0 || strcmp(type_str, "mode9") == 0)
                mode = 9;
            else if (strcmp(type_str, "MODE10") == 0 || strcmp(type_str, "mode10") == 0)
                mode = 10;
            else if (strcmp(type_str, "MODE11") == 0 || strcmp(type_str, "mode11") == 0)
                mode = 11;
            else if (strcmp(type_str, "MODE12") == 0 || strcmp(type_str, "mode12") == 0)
                mode = 12;
            else
                break;

            LOGI("mode %d", mode);

            vclk_phase[mode] = vclk_phase_default[mode];

            if ((value >> 0) & 1) // 0x8e[0]
                vclk_phase[mode] ^= (1 << 16);

            if ((value >> 1) & 1) // 0x8e[5]
                vclk_phase[mode] ^= (1 << 21);

            LOGI("%s 0x%02x, ori:0x%08x new:0x%08x", type_str, value, vclk_phase_default[mode], vclk_phase[mode]);
        }
    }

    fclose(file);
    return 0;
}

uint8_t vclk_phase_write_file(char *file_path, uint32_t phase_p[]) {
    FILE *file;

    LOGI("vclk_phase_write_file %s", file_path);

    file = fopen(file_path, "w");
    if (file == NULL) {
        LOGI("%s open failed", file_path);
        return 1;
    }

    fprintf(file, "VIDEO_SOURCE_VERSION 0x%08x\r\n", phase_p[VIDEO_SOURCE_VERSION]);
    fprintf(file, "VIDEO_SOURCE_MENU_UI 0x%08x\r\n", phase_p[VIDEO_SOURCE_MENU_UI]);
    fprintf(file, "VIDEO_SOURCE_HDZERO_IN_720P60_50 0x%08x\r\n", phase_p[VIDEO_SOURCE_HDZERO_IN_720P60_50]);
    fprintf(file, "VIDEO_SOURCE_HDZERO_IN_720P90 0x%08x\r\n", phase_p[VIDEO_SOURCE_HDZERO_IN_720P90]);
    fprintf(file, "VIDEO_SOURCE_HDZERO_IN_1080P30 0x%08x\r\n", phase_p[VIDEO_SOURCE_HDZERO_IN_1080P30]);
    fprintf(file, "VIDEO_SOURCE_AV_IN 0x%08x\r\n", phase_p[VIDEO_SOURCE_AV_IN]);
    fprintf(file, "VIDEO_SOURCE_HDMI_IN_1080P50 0x%08x\r\n", phase_p[VIDEO_SOURCE_HDMI_IN_1080P50]);
    fprintf(file, "VIDEO_SOURCE_HDMI_IN_1080P60 0x%08x\r\n", phase_p[VIDEO_SOURCE_HDMI_IN_1080P60]);
    fprintf(file, "VIDEO_SOURCE_HDMI_IN_1080POTHER 0x%08x\r\n", phase_p[VIDEO_SOURCE_HDMI_IN_1080POTHER]);
    fprintf(file, "VIDEO_SOURCE_HDMI_IN_720P50 0x%08x\r\n", phase_p[VIDEO_SOURCE_HDMI_IN_720P50]);
    fprintf(file, "VIDEO_SOURCE_HDMI_IN_720P60 0x%08x\r\n", phase_p[VIDEO_SOURCE_HDMI_IN_720P60]);
    fprintf(file, "VIDEO_SOURCE_HDMI_IN_720P100 0x%08x\r\n", phase_p[VIDEO_SOURCE_HDMI_IN_720P100]);
    fprintf(file, "VIDEO_SOURCE_TP2825_EX 0x%08x\r\n", phase_p[VIDEO_SOURCE_TP2825_EX]);

    fclose(file);
}
uint8_t pclk_phase_write_file(char *file_path, uint32_t phase_p[]) {
    FILE *file;

    LOGI("pclk_phase_write_file %s", file_path);

    file = fopen(file_path, "w");
    if (file == NULL) {
        LOGI("%s open failed", file_path);
        return 1;
    }

    fprintf(file, "VIDEO_SOURCE_VERSION 0x%08x\r\n", phase_p[VIDEO_SOURCE_VERSION]);
    fprintf(file, "VIDEO_SOURCE_MENU_UI 0x%08x\r\n", phase_p[VIDEO_SOURCE_MENU_UI]);
    fprintf(file, "VIDEO_SOURCE_HDZERO_IN_720P60_50 0x%08x\r\n", phase_p[VIDEO_SOURCE_HDZERO_IN_720P60_50]);
    fprintf(file, "VIDEO_SOURCE_HDZERO_IN_720P90 0x%08x\r\n", phase_p[VIDEO_SOURCE_HDZERO_IN_720P90]);
    fprintf(file, "VIDEO_SOURCE_HDZERO_IN_1080P30 0x%08x\r\n", phase_p[VIDEO_SOURCE_HDZERO_IN_1080P30]);
    fprintf(file, "VIDEO_SOURCE_AV_IN 0x%08x\r\n", phase_p[VIDEO_SOURCE_AV_IN]);
    fprintf(file, "VIDEO_SOURCE_HDMI_IN_1080P50 0x%08x\r\n", phase_p[VIDEO_SOURCE_HDMI_IN_1080P50]);
    fprintf(file, "VIDEO_SOURCE_HDMI_IN_1080P60 0x%08x\r\n", phase_p[VIDEO_SOURCE_HDMI_IN_1080P60]);
    fprintf(file, "VIDEO_SOURCE_HDMI_IN_1080POTHER 0x%08x\r\n", phase_p[VIDEO_SOURCE_HDMI_IN_1080POTHER]);
    fprintf(file, "VIDEO_SOURCE_HDMI_IN_720P50 0x%08x\r\n", phase_p[VIDEO_SOURCE_HDMI_IN_720P50]);
    fprintf(file, "VIDEO_SOURCE_HDMI_IN_720P60 0x%08x\r\n", phase_p[VIDEO_SOURCE_HDMI_IN_720P60]);
    fprintf(file, "VIDEO_SOURCE_HDMI_IN_720P100 0x%08x\r\n", phase_p[VIDEO_SOURCE_HDMI_IN_720P100]);
    fprintf(file, "VIDEO_SOURCE_TP2825_EX 0x%08x\r\n", phase_p[VIDEO_SOURCE_TP2825_EX]);

    fclose(file);
}

void vclk_phase_load_system() {
    uint8_t i;

    if (vclk_phase_read_file("/etc/vclk_phase.cfg")) {
        // if no .cfg file, write it.
        vclk_phase_write_file("/etc/vclk_phase.cfg", vclk_phase_default);

        for (i = 0; i < VIDEO_SOURCE_NUM; i++) {
            vclk_phase[i] = vclk_phase_default[i];
        }
    } else if (vclk_phase_load[VIDEO_SOURCE_VERSION] != 0xffffffff && vclk_phase_load[VIDEO_SOURCE_VERSION] != vclk_phase_default[VIDEO_SOURCE_VERSION]) {
        // newer .cfg file version
        vclk_phase_write_file("/etc/vclk_phase.cfg", vclk_phase_default);

        for (i = 0; i < VIDEO_SOURCE_NUM; i++) {
            vclk_phase[i] = vclk_phase_default[i];
        }
    } else {
        for (i = 0; i < VIDEO_SOURCE_NUM; i++) {
            if (vclk_phase_load[i] != 0xffffffff)
                vclk_phase[i] = vclk_phase_load[i];
        }
    }
}

void pclk_phase_load_system() {
    uint8_t i;

    system_exec("cp /mnt/extsd/pclk_phase.cfg /etc/pclk_phase.cfg");
    system_exec("rm /mnt/extsd/pclk_phase.cfg");

    if (pclk_phase_read_file("/etc/pclk_phase.cfg")) {
        // if no .cfg file, write it.
        pclk_phase_write_file("/etc/pclk_phase.cfg", pclk_phase_default);

        for (i = 0; i < VIDEO_SOURCE_NUM; i++) {
            pclk_phase[i] = pclk_phase_default[i];
        }
    } else if (pclk_phase_load[VIDEO_SOURCE_VERSION] != 0xffffffff && pclk_phase_load[VIDEO_SOURCE_VERSION] != pclk_phase_default[VIDEO_SOURCE_VERSION]) {
        // newer .cfg file version
        pclk_phase_write_file("/etc/pclk_phase.cfg", pclk_phase_default);

        for (i = 0; i < VIDEO_SOURCE_NUM; i++) {
            pclk_phase[i] = pclk_phase_default[i];
        }
    } else {
        for (i = 0; i < VIDEO_SOURCE_NUM; i++) {
            if (pclk_phase_load[i] != 0xffffffff)
                pclk_phase[i] = pclk_phase_load[i];
        }
    }
}

void vclk_phase_inv_load_sdcard() {
    if (vclk_phase_inv_read_file("/mnt/extsd/vclk_phase_inv.txt")) {
        return;
    }

    vclk_phase_write_file("/etc/vclk_phase.cfg", vclk_phase);

    system_exec("rm /mnt/extsd/vclk_phase_inv.txt");
}

void vclk_phase_dump() {
    FILE *file;
    char *file_path = "/mnt/extsd/vclk_phase_dump.cfg";

    file = fopen(file_path, "r");
    if (file == NULL)
        return;
    fclose(file);

    vclk_phase_write_file(file_path, vclk_phase);
}
void pclk_phase_dump() {
    FILE *file;
    char *file_path = "/mnt/extsd/pclk_phase_dump.cfg";

    file = fopen(file_path, "r");
    if (file == NULL)
        return;
    fclose(file);

    pclk_phase_write_file(file_path, pclk_phase);
}

void vclk_phase_init() {
    vclk_phase_load_system();
    vclk_phase_inv_load_sdcard();
    vclk_phase_dump();
}

void pclk_phase_init() {
    pclk_phase_load_system();
    pclk_phase_dump();
}

// The clock phases above are for the FPGA that 9.6 ships (VA 198). Running
// from the SD card never flashes it, so a goggle on 9.5.1 keeps its older
// FPGA (VA 176 on the one tested), where HDMI in needs 9.5.1's settings: with
// these it showed no picture from a Mac that stock showed fine. AV (both
// analog inputs) showed its picture but none of the goggle's OSD over it, and
// the OSD layer is the SoC's output clocked into the FPGA, which is exactly
// what these settings are about, so it goes back to 9.5.1's too. HDZero works
// on the old FPGA as it is and is left alone.
#define FPGA_VA_NEW_CLOCKS 198

static bool fpga_old_clocks(void) {
    static int old = -1;

    if (old < 0) {
        int va = I2C_Read(ADDR_FPGA, 0xff);

        old = va < FPGA_VA_NEW_CLOCKS;
        LOGI("fpga: va %d, %s's where the two differ (HDMI in and AV clocks, 1080p24, HAN check)",
             va, old ? "9.5.1" : "9.6");
    }
    return old;
}

static bool is_hdmi_in_source(video_source_t source) {
    return source >= VIDEO_SOURCE_HDMI_IN_1080P50 && source <= VIDEO_SOURCE_HDMI_IN_720P100;
}

static bool is_old_clock_source(video_source_t source) {
    return (is_hdmi_in_source(source) || source == VIDEO_SOURCE_AV_IN) && fpga_old_clocks();
}

// 9.5.1's FPGA 0x8d for HDMI in (its 0x8e is 0x04 for all of them).
static uint8_t old_hdmi_in_8d(video_source_t source) {
    return (source == VIDEO_SOURCE_HDMI_IN_720P50 || source == VIDEO_SOURCE_HDMI_IN_720P60) ? 0x00 : 0x10;
}

// 9.6 never writes 0x8d, so the old-FPGA paths put back what they found
// when any other source takes over, leaving those exactly as they were.
static int fpga_8d_saved = -1;

void vclk_phase_set(video_source_t source, uint8_t reg_8d_sel) {
    if (is_old_clock_source(source)) {
        if (fpga_8d_saved < 0)
            fpga_8d_saved = I2C_Read(ADDR_FPGA, 0x8d);
        if (source == VIDEO_SOURCE_AV_IN) {
            // 9.5.1: 0x10840000, 0x8d 0x10 and 0x8e 0x84.
            I2C_Write(ADDR_FPGA, 0x8d, 0x10 ^ (reg_8d_sel ? 0x10 : 0x00));
            I2C_Write(ADDR_FPGA, 0x8e, 0x84);
        } else {
            I2C_Write(ADDR_FPGA, 0x8d, old_hdmi_in_8d(source) ^ (reg_8d_sel ? 0x10 : 0x00));
            I2C_Write(ADDR_FPGA, 0x8e, 0x04);
        }
        return;
    }

    if (fpga_8d_saved >= 0) {
        I2C_Write(ADDR_FPGA, 0x8d, fpga_8d_saved);
        fpga_8d_saved = -1;
    }
    I2C_Write(ADDR_FPGA, 0x8e, (vclk_phase[source] >> 16) & 0xff);
}

/*
DCLK_INVERT
    0: Disable dclk invert
    1: Enable dclk invert
DCLK_DLY_EN
    0: Disable
    1: Enable
DCLK_DLY_NUM[5:0]
    Number= num+1
*/
void vdpo_sync_ctrl_set(bool dclk_invert, bool dclk_dly_en, uint8_t dvlk_dly_num) {
    const uint32_t addr = 0x06542008;
    uint32_t dat = 0x00000003;
    char buf[32];

    dat |= (dclk_invert << 3);
    dat |= (dvlk_dly_num << 4);
    dat |= (dclk_dly_en << 10);

    snprintf(buf, sizeof(buf), "aww 0x%08x 0x%08x", addr, dat);
    system_exec(buf);
}

/*
used for dvr
Pclk_dly
default value : 9
32 Step for adjust, 1 step = 0.2ns
*/
void csic_pclk_dly_set(uint8_t pclk_dly_num) {
    const uint32_t addr = 0x06601500;
    uint32_t dat = pclk_dly_num;
    char buf[32];

    snprintf(buf, sizeof(buf), "aww 0x%08x 0x%08x", addr, dat);
    system_exec(buf);
}

void csic_pclk_invert_set(uint8_t is_invert) {
    const uint32_t addr = 0x06601004;
    uint32_t dat = is_invert ? 0x010000A0 : 0x010100A0;
    char buf[32];

    snprintf(buf, sizeof(buf), "aww 0x%08x 0x%08x", addr, dat);
    system_exec(buf);
}

void pclk_phase_set(video_source_t source) {
    const bool old_hdmi_in = is_hdmi_in_source(source) && fpga_old_clocks();
    const bool old_av = source == VIDEO_SOURCE_AV_IN && fpga_old_clocks();

    LOGI("video_source:%d", source);
    LOGI("pclk_phase_set %d%s", pclk_phase[source],
         old_hdmi_in ? " (9.5.1 HDMI in)" : old_av ? " (9.5.1 AV)" : "");
    // bit[0] hdmi in
    // IT66021_Set_Pclk((pclk_phase[source] >> 0) & 1, 0);
    IT66021_Mask_WR(0, 0x0f, 0x03, 0x00);
    if (old_hdmi_in)
        IT66021_Mask_WR(0, 0x50, 0x1f, 0x12); // as IT66021_init() leaves it, which 9.5.1 never changed
    else if ((pclk_phase[source] >> 0) & 1)
        I2C_L_Write(ADDR_IT66021, 0x50, 0xb3);
    else
        I2C_L_Write(ADDR_IT66021, 0x50, 0xa1);

    // bit[1] analog in
    TP2825_Set_Pclk((pclk_phase[source] >> 1) & 1);

    // bit[2] osd (9.5.1 set it for every source on goggle 2, and for AV with
    // the clock delay on as well)
    vdpo_sync_ctrl_set(old_hdmi_in || old_av || ((pclk_phase[source] >> 2) & 1), old_av, 0);

    // bit[3] dvr
    csic_pclk_invert_set((pclk_phase[source] >> 3) & 1);

    // bit [5:4] hdmi out  bit[7]
    IT66121_set_phase((pclk_phase[source] >> 4) & 3, (pclk_phase[source] >> 7) & 1);

    // bit[6] oled
    I2C_Write(ADDR_AL, 0x14, (pclk_phase[source] >> 6) & 1);
}
void hw_stat_init() {
    g_hw_stat.source_mode = SOURCE_MODE_UI;
    g_hw_stat.vdpo_tmg = VDPO_TMG_1080P50;

    g_hw_stat.hdz_bw = 0;
    g_hw_stat.hdzero_open = 0;
    g_hw_stat.hdz_standby = 0;
    g_hw_stat.m0_open = 0;

    g_hw_stat.is_av_in = 1;
    g_hw_stat.av_pal[0] = g_hw_stat.av_pal[1] = g_hw_stat.av_pal_w = 0;
    g_hw_stat.av_valid[0] = g_hw_stat.av_valid[1] = 0;

    g_hw_stat.hdmiin_valid = 0;
    g_hw_stat.hdmiin_vtmg = HDMIIN_VTMG_1080P50;
    g_hw_stat.IS_TP2825_L = 0;

    pthread_mutex_init(&hardware_mutex, NULL);
}

void Display_VO_SWITCH(uint8_t sel) // 0 = UI;  1 = HDZERO or AV_in or HDMI_in
{
    I2C_Write(ADDR_FPGA, 0x20, sel);

    if (sel && g_hw_stat.hdzero_open && (g_hw_stat.source_mode == SOURCE_MODE_HDZERO))
        DM6302_openM0(1);
    else if (!HDZero_open_pending())
        // While an async open is outstanding this would push an SPI sequence
        // into the middle of the init for no gain: DM6302_M0() leaves the
        // register at zero anyway, so the boot-time close from
        // Display_UI_init() has nothing to add.
        DM6302_openM0(0);

    I2C_Write(ADDR_FPGA, 0x06, 0x0F);
}

void hw_screen_on(int bON) {
    pthread_mutex_lock(&hardware_mutex);
    screen.display(bON);
    pthread_mutex_unlock(&hardware_mutex);
}

// The player's display is up (Display_Playback()): its lines, 720 or 1080,
// or 0. The menu's display ends it.
static int playback_display = 0;

void Display_UI_init() {
    // The boot call configures the display for the menu, and the switch to the
    // last source immediately reconfigures it for the video, paying dispw's
    // second measured at over a second twice over. Skipping the first one
    // leaves the boot UI on whatever mode the kernel set up.
    static bool first_call = true;
    bool skip_timing = first_call && g_setting.speed.boot_display;

    first_call = false;
    playback_display = 0;

    g_hw_stat.source_mode = SOURCE_MODE_UI;
    I2C_Write(ADDR_FPGA, 0x8C, 0x00);

    // 1080p50 on this side of the mux (the player gets to 1080p60 on the
    // other: Display_Playback()). The recordings are 720p60, and
    // since the decoder's 40fps drop was lifted (a frame interval in
    // libmpp_vo.so) 60 frames a second reach this panel and can only be shown
    // 6:5 at 50Hz. Two ways to a 60Hz panel were measured here and both
    // corrupted the picture, each in the way its own mismatch predicts:
    //
    //   1080p60 -- right resolution, wrong rate -- tore it diagonally. Tried
    //   with the MENU_UI clock phase and again with
    //   VIDEO_SOURCE_HDZERO_IN_1080P30's, whose path genuinely runs the VDPO
    //   at 1080p60, so the phase is not what is missing.
    //
    //   720p60 -- wrong resolution -- repeated it horizontally and sheared it.
    //   The player's own video layer was correct and ran at a true 60fps, so
    //   what breaks is the framebuffer half of the display, not the timing.
    //
    // Both fit the FPGA holding a fixed geometry for this input. The live
    // video takes the other side of the mux (Display_VO_SWITCH(1)) and the app
    // programs its input timing explicitly, which is why 720p60 works there:
    // screen.mfpga.set720p60() writes a horizontal total of 1650, and
    // MFPGA_Set1080P30() writes 2200 for its 1080p60 output. There is no
    // MFPGA_Set1080P50() at all, so nothing here ever sets a geometry for the
    // framebuffer path -- it is configured outside this program and expects
    // 1080p50. Changing that needs the FPGA registers for this input, which
    // are undocumented.
    if (!skip_timing)
        vdpo_set_timing(VDPO_TMG_1080P50, "1080p50");
    system_exec("aww 0x0300b340 0x00000008");
    Display_VO_SWITCH(0);

    vclk_phase_set(VIDEO_SOURCE_MENU_UI, 0);
    pclk_phase_set(VIDEO_SOURCE_MENU_UI);
    I2C_Write(ADDR_FPGA, 0x80, 0x00);
    I2C_Write(ADDR_FPGA, 0x84, 0x11);

    screen.vtmg(0);
    system_exec("aww 0x0300b084 0x00003fff"); // Set vdpo clock driver strength to level 2. Refer datasheet 12.7.5.11
    I2C_Write(ADDR_FPGA, 0xa7, 0x00);
    system_exec("aww 0x06542018 0x00000044"); // disable horizontal chroma FIR filter.
}

void Display_UI_then(void (*before_on)(void)) {
    pthread_mutex_lock(&hardware_mutex);
    LOGI("switch mark: Display_UI start");
    screen.display(0);
    LOGI("switch mark: oled off");
    Display_UI_init();
    LOGI("switch mark: vdpo reconfigured");

    if (before_on)
        before_on();
    screen.display(1);
    LOGI("switch mark: oled on");
    pthread_mutex_unlock(&hardware_mutex);
}

void Display_UI() {
    Display_UI_then(NULL);
}

// The player's display: 60Hz, so that recordings made at 60fps are shown
// frame for frame. The menu's 1080p50 shows them 6:5, ten frames a second
// never reaching the panel.
//
// It goes through the live side of the FPGA mux. The UI side seems to hold a
// geometry of its own for 1080p50: at 60Hz it tore the picture diagonally
// (1080p60, with the menu's clock phases and again with the 1080p30 camera's
// phases, FPGA 0x80 and geometry) and at 720p60 it showed four copies across
// with the lines shifted (with and without the 720p60 geometry). The live
// side takes 720p60 and 1080p60 from the SoC every day, as the 720p60 and
// 1080p30 cameras run the display, with the SoC's output as the OSD over the
// live picture. With the receiver off the live picture is black, and the
// SoC's output -- the player's video layer and LVGL's -- is the whole screen.
//
// Except where the SoC's output is the OSD's transparent grey: the FPGA shows
// the live layer there (magenta with the receiver off, at 1080p, black at
// 720p). The player keeps its frames off that grey (avplay.c), which it can
// only do for a frame that reaches the output pixel for pixel: hence a mode
// the recording's size, 720 lines for a 720p recording (the FPGA scales it to
// the panel, as it does the 720p60 camera's) and 1080 for a 1080p one.
//
// Everything is as Display_720P60_50_t() or Display_1080P30_t() has it except
// the receiver, which is left alone: the source stays the UI's, so
// Display_VO_SWITCH() does not open it. Display_UI_init(), which every return
// to the menu goes through, ends it.
void Display_Playback(int lines) {
    const bool hd = lines != 720;

    pthread_mutex_lock(&hardware_mutex);
    LOGI("switch mark: Display_Playback %d start", hd ? 1080 : 720);
    screen.display(0);

    g_hw_stat.source_mode = SOURCE_MODE_UI;
    I2C_Write(ADDR_FPGA, 0x8C, 0x00);
    if (hd) {
        vdpo_set_timing(VDPO_TMG_1080P60, "1080p60");
    } else {
        vdpo_set_timing(VDPO_TMG_720P60, "720p60");
    }
    system_exec("aww 0x0300b340 0x00000008");

    const video_source_t phases = hd ? VIDEO_SOURCE_HDZERO_IN_1080P30 : VIDEO_SOURCE_HDZERO_IN_720P60_50;
    vclk_phase_set(phases, 0);
    pclk_phase_set(phases);
    I2C_Write(ADDR_FPGA, 0x80, hd ? 0x04 : 0x00);
    I2C_Write(ADDR_FPGA, 0x84, 0x11); // the SoC's output over the live picture

    if (hd) {
        screen.mfpga.set1080p30();
        screen.vtmg(0);
    } else {
        screen.mfpga.set720p60(VR_720P60, 0);
        screen.vtmg(1);
    }
    system_exec("aww 0x0300b084 0x00003fff");
    I2C_Write(ADDR_FPGA, 0xa7, hd ? 0x00 : 0x11);
    I2C_Write(ADDR_FPGA, 0x8C, 0x01);
    Display_VO_SWITCH(1);
    system_exec("aww 0x06542018 0x00000044");

    // The panel stays dark: the player lights it (hw_screen_on()) once MPP is
    // up. MPP puts the UI layer back to 1920x1080 as it starts, and on the
    // 720-line display that layer comes out with its lines sheared until the
    // player fits it to the screen again: lit here, that showed for half a
    // second (480ms from here to the fit in the log) at every playback start.
    playback_display = hd ? 1080 : 720;
    LOGI("switch mark: Display_Playback done, panel left off");
    pthread_mutex_unlock(&hardware_mutex);
}

int Display_Playback_lines(void) {
    return g_hw_stat.source_mode == SOURCE_MODE_UI ? playback_display : 0;
}

void Display_720P60_50_t(int mode, uint8_t is_43) // fps: 0=50, 1=60
{
    screen.display(0);
    I2C_Write(ADDR_FPGA, 0x8C, 0x00);

    vdpo_set_timing(VDPO_TMG_720P60, "720p60");
    vclk_phase_set(VIDEO_SOURCE_HDZERO_IN_720P60_50, 0);
    pclk_phase_set(VIDEO_SOURCE_HDZERO_IN_720P60_50);

    I2C_Write(ADDR_FPGA, 0x80, (mode == VR_540P60) ? 0x01 : 0x00);

    DM5680_SetFPS(mode);
    if (mode == VR_540P60) {
        screen.mfpga.set540p60();
    } else {
        screen.mfpga.set720p60(mode, is_43); // is_43 = 1 for 4:3, 0 for 16:9
    }

    screen.vtmg(1);

    I2C_Write(ADDR_FPGA, 0xa7, 0x11);
    I2C_Write(ADDR_FPGA, 0x8C, 0x01);

    g_hw_stat.source_mode = SOURCE_MODE_HDZERO;
    Display_VO_SWITCH(1);
    screen.display(1);
    system_exec("aww 0x06542018 0x00000044"); // disable horizontal chroma FIR filter.
}

void Display_720P90_t(int mode) {
    screen.display(0);
    I2C_Write(ADDR_FPGA, 0x8C, 0x00);

    vdpo_set_timing(VDPO_TMG_720P90, "720p90");
    vclk_phase_set(VIDEO_SOURCE_HDZERO_IN_720P90, 0);
    pclk_phase_set(VIDEO_SOURCE_HDZERO_IN_720P90);
    I2C_Write(ADDR_FPGA, 0x80, 0x03);

    DM5680_SetFPS(mode);
    screen.mfpga.set720p90(mode);
    screen.vtmg(1);

    I2C_Write(ADDR_FPGA, 0xa7, 0x01);
    I2C_Write(ADDR_FPGA, 0x8C, 0x01);

    g_hw_stat.source_mode = SOURCE_MODE_HDZERO;
    Display_VO_SWITCH(1);
    screen.display(1);
    system_exec("aww 0x06542018 0x00000044"); // disable horizontal chroma FIR filter.
}

void Display_1080P30_t(int mode) {
    screen.display(0);
    I2C_Write(ADDR_FPGA, 0x8C, 0x00);

    vdpo_set_timing(VDPO_TMG_1080P60, "1080p60");
    vclk_phase_set(VIDEO_SOURCE_HDZERO_IN_1080P30, 0);
    pclk_phase_set(VIDEO_SOURCE_HDZERO_IN_1080P30);

    I2C_Write(ADDR_FPGA, 0x80, 0x04);

    DM5680_SetFPS(mode);
    screen.mfpga.set1080p30();
    screen.vtmg(0);

    I2C_Write(ADDR_FPGA, 0xa7, 0x00);
    I2C_Write(ADDR_FPGA, 0x8C, 0x01);

    g_hw_stat.source_mode = SOURCE_MODE_HDZERO;
    Display_VO_SWITCH(1);
    screen.display(1);
    system_exec("aww 0x06542018 0x00000044"); // disable horizontal chroma FIR filter.
}

void Display_1080P24_t(int mode) {
    screen.display(0);
    I2C_Write(ADDR_FPGA, 0x8C, 0x00);

    vdpo_set_timing(VDPO_TMG_1080P60, "1080p60");
    vclk_phase_set(VIDEO_SOURCE_HDZERO_IN_1080P30, 0);
    pclk_phase_set(VIDEO_SOURCE_HDZERO_IN_1080P30);

    // 9.6 changed this from 0x84 along with its FPGA; the older FPGA keeps
    // 9.5.1's.
    I2C_Write(ADDR_FPGA, 0x80, fpga_old_clocks() ? 0x84 : 0x04);
    // I2C_Write(ADDR_FPGA, 0x84, 0x00); // close OSD

    DM5680_SetFPS(mode);
    screen.mfpga.set1080p30();
    screen.vtmg(0);

    I2C_Write(ADDR_FPGA, 0xa7, 0x00);
    I2C_Write(ADDR_FPGA, 0x8C, 0x01);

    g_hw_stat.source_mode = SOURCE_MODE_HDZERO;
    Display_VO_SWITCH(1);
    screen.display(1);
    system_exec("aww 0x06542018 0x00000044"); // disable horizontal chroma FIR filter.
}

void Display_720P60_50(int mode, uint8_t is_43) {
    pthread_mutex_lock(&hardware_mutex);
    Display_720P60_50_t(mode, is_43);
    pthread_mutex_unlock(&hardware_mutex);
}

void Display_720P90(int mode) {
    pthread_mutex_lock(&hardware_mutex);
    Display_720P90_t(mode);
    pthread_mutex_unlock(&hardware_mutex);
}

void Display_1080P24(int mode) {
    pthread_mutex_lock(&hardware_mutex);
    Display_1080P24_t(mode);
    pthread_mutex_unlock(&hardware_mutex);
}

void Display_1080P30(int mode) {
    pthread_mutex_lock(&hardware_mutex);
    Display_1080P30_t(mode);
    pthread_mutex_unlock(&hardware_mutex);
}

int HDZERO_detect() // return = 1: vtmg to V536 changed
{
    static video_resolution_t cam_mode_last = VR_720P60;
    static uint8_t cam_4_3_last = 0;
    int ret = 0;

    pthread_mutex_lock(&hardware_mutex);

    if (g_hw_stat.source_mode == SOURCE_MODE_HDZERO) {

        if (cam_mode_last != CAM_MODE) { // Camera mode changed
            LOGI("CAM_mode: %d->%d", cam_mode_last, CAM_MODE);

            // 1. Change fps
            switch (CAM_MODE) {
            case VR_720P50:
            case VR_720P60:
            case VR_960x720P60:
            case VR_540P60:
                Display_720P60_50_t(CAM_MODE, cam_4_3);
                break;
            case VR_720P30:
                perror("cam_mode =2 fix me!!\n ");
                break;
            case VR_540P90:
            case VR_540P90_CROP:
                Display_720P90_t(CAM_MODE);
                break;
            case VR_1080P30:
                Display_1080P30_t(CAM_MODE);
                break;
            case VR_1080P24:
                Display_1080P24_t(CAM_MODE);
                break;
            default:
                LOGW("cam_mode =%d not suppored!!\n ", CAM_MODE);
                break;
            }

            if (CAM_MODE == VR_1080P30 || CAM_MODE == VR_1080P24)
                fhd_req = 1;
            else if (cam_mode_last == VR_1080P30 || cam_mode_last == VR_1080P24)
                fhd_req = -1;
            dvr_update_vi_conf(CAM_MODE);
            system_script(REC_STOP_LIVE);
            cam_mode_last = CAM_MODE;
            ret = 1;
        }

        if (cam_4_3_last != cam_4_3) {
            LOGI("cam_4_3: %d  CAM_MODE=%d", cam_4_3, CAM_MODE);
            cam_4_3_last = cam_4_3;

            if (CAM_MODE != VR_720P50) {
                screen.mfpga.set_ratio(cam_4_3); // set ratio for 720P60/90/1080P30
            } else {
                screen.mfpga.set_ratio(0);
            }
        }
    }
    pthread_mutex_unlock(&hardware_mutex);
    return ret;
}

void AV_Mode_Switch_fpga(int is_pal) {
    if (is_pal) {
        vdpo_set_timing(VDPO_TMG_720P50, "720p50");
        I2C_Write(ADDR_FPGA, 0x80, 0x10);
    } else {
        vdpo_set_timing(VDPO_TMG_720P60, "720p60");
        I2C_Write(ADDR_FPGA, 0x80, 0x00);
    }
    I2C_Write(ADDR_FPGA, 0x06, 0x0F);
    system_exec("aww 0x06542018 0x00000044"); // disable horizontal chroma FIR filter.
    // 9.5.1 ended here with the OSD clock as AV has it, which an NTSC/PAL
    // change (AV_Mode_Switch(), with no pclk_phase_set() after it) relies on.
    if (fpga_old_clocks())
        vdpo_sync_ctrl_set(true, true, 0);
}

void AV_Mode_Switch(int is_pal) {
    int tmg = is_pal ? VDPO_TMG_720P50 : VDPO_TMG_720P60;

    if (g_hw_stat.vdpo_tmg != tmg) {
        screen.display(0);
        AV_Mode_Switch_fpga(is_pal);
        screen.display(1);
    }
}

void Source_AV(bool is_av_in) {
    pthread_mutex_lock(&hardware_mutex);
    screen.display(0);
    I2C_Write(ADDR_FPGA, 0x8C, 0x00);

    g_hw_stat.is_av_in = is_av_in;

    TP2825_init(is_av_in, g_setting.source.analog_format);

    TP2825_Switch_Mode(g_setting.source.analog_format);
    TP2825_Switch_CH(g_hw_stat.is_av_in);

    AV_Mode_Switch_fpga(g_setting.source.analog_format);
    g_hw_stat.av_pal_w = g_setting.source.analog_format;

    vclk_phase_set(VIDEO_SOURCE_AV_IN, 0);
    pclk_phase_set(VIDEO_SOURCE_AV_IN);

    I2C_Write(ADDR_FPGA, 0x89, 0x01);

    HDZero_Close();
    screen.vtmg(1);
    I2C_Write(ADDR_FPGA, 0xa7, 0x11);

    if (g_setting.source.analog_ratio == SETTING_SOURCES_ANALOG_RATIO_4_3)
        I2C_Write(ADDR_FPGA, 0x8f, 0x80); // bit[7]: 0=15:9, 1=original
    else
        I2C_Write(ADDR_FPGA, 0x8f, 0x00); // bit[7]: 0=15:9, 1=original

    I2C_Write(ADDR_FPGA, 0x8C, 0x02);

    g_hw_stat.source_mode = SOURCE_MODE_AV;
    Display_VO_SWITCH(1);
    screen.display(1);

    pthread_mutex_unlock(&hardware_mutex);
}

#define AV_DET_SWITCH_CNT 1
// Reads to throw away after pointing the decoder at the other input. Its lock
// bits say nothing true about a signal it has only just been connected to,
// and at roughly 100ms a poll this is about a third of a second before the two
// agreeing reads below are allowed to decide anything. Without it both could
// land inside the settling time, which committed "no signal" for an input that
// had one and switched away from it: the Source page's rows blinking, or
// staying dark (seen on a goggle v1; the goggle 2 fork had the same fix).
#define AV_SCAN_SETTLE    3
#define AV_DET_LOCK_CNT   10
#define AV_DET_DROP_CNT   5
int AV_in_detect() // return = 1: vtmg to V536 changed
{
    static int det_last = -1;
    static int det_cnt = 0, det2_cnt = 0;
    static int settle = AV_SCAN_SETTLE;
    int rdat, det;
    int ret = 0;

    pthread_mutex_lock(&hardware_mutex);

    rdat = I2C_Read(ADDR_TP2825, 0x01);

    // Both analog inputs are scanned by switching the decoder between them, so
    // this can only run while nothing watches the decoder's output. Stock
    // asked for the menu owning the display, which Menu Over Video breaks (it
    // leaves the display on the live source, and the rows then kept the zeros
    // hw_stat_init() gave them). The decoder is free whenever the picture on
    // screen is not the analog one, menu overlaid or not.
    bool decoder_idle = (g_hw_stat.source_mode == SOURCE_MODE_UI) ||
                        (app_menu_is_over_video() && g_hw_stat.source_mode != SOURCE_MODE_AV);

    if (decoder_idle) { // scan both inputs
        // The video-loss bit on its own flaps, so the rows blinked between
        // connected and disconnected while the same input played fine when
        // selected. Ask whether the decoder has locked (as upstream 9.5.1 did
        // on the goggle 2).
        int vloss = ((rdat & 0x80) == 0x80);
        int h_lock = ((rdat & 0x60) == 0x60);
        int vh_lock = ((rdat & 0x68) == 0x68);

        det = (vloss == 0) && (vh_lock || h_lock);

        if (settle > 0) {
            settle--;
        } else if (det_last != det) {
            det_last = det;
        } else {
            // What the status register says on each input, for the first
            // stretch of a boot and at every change after that: two inputs at
            // about a second a round trip, so a couple of minutes, then quiet.
            static int diag_left = 200;
            bool changed = g_hw_stat.av_valid[g_hw_stat.is_av_in] != det;

            if (changed || diag_left > 0) {
                if (diag_left > 0)
                    diag_left--;
                LOGI("av: %s %s reg01=%02x%s", g_hw_stat.is_av_in ? "avin" : "bay ",
                     det ? "signal" : "none  ", rdat, changed ? " changed" : "");
            }

            g_hw_stat.av_valid[g_hw_stat.is_av_in] = det;
            g_hw_stat.av_pal[g_hw_stat.is_av_in] = (rdat & 0x01) ? 0 : 1;

            g_hw_stat.is_av_in = 1 - g_hw_stat.is_av_in;
            TP2825_Switch_CH(g_hw_stat.is_av_in);
            det_last = -1;
            settle = AV_SCAN_SETTLE;
        }

        det_cnt = det2_cnt = 0;
    } else if (g_hw_stat.source_mode == SOURCE_MODE_AV) { // detect in AV_in/Module_bay mode
        settle = AV_SCAN_SETTLE;
        det = ((rdat & 0xC9) == (g_hw_stat.av_pal_w ? 0x48 : 0x49)) ? 1 : 0;

        if (det_last != det) {
            det_last = det;
            det_cnt = 0;
        } else if (det_cnt < AV_DET_SWITCH_CNT) {
            det_cnt++;
        }

        if (det && det_cnt == AV_DET_SWITCH_CNT) {
            g_hw_stat.av_pal_w = g_hw_stat.av_pal_w ? 0 : 1;

            TP2825_Switch_Mode(g_hw_stat.av_pal_w);
            AV_Mode_Switch(g_hw_stat.av_pal_w);

            if (g_hw_stat.av_pal[g_hw_stat.is_av_in])
                I2C_Write(ADDR_FPGA, 0x80, 0x10);
            else
                I2C_Write(ADDR_FPGA, 0x80, 0x00);

            g_hw_stat.av_valid[g_hw_stat.is_av_in] = 0;
            ret = 1;

            LOGI("AV_in_detect -- switch: av_pal = %d,  rdat = %02x\n", g_hw_stat.av_pal_w, rdat);
        } else {
            int vloss, h_lock, vh_lock;

            vloss = ((rdat & 0x80) == 0x80);
            h_lock = ((rdat & 0x60) == 0x60);
            vh_lock = ((rdat & 0x68) == 0x68);
            vloss |= ((rdat & 0x6a) == 0); // vloss = 0 if no signal ???

            // LOGI("rdat=%x, state=%d, dcnt2=%d", rdat,g_hw_stat.av_valid[g_hw_stat.is_av_in],det2_cnt);

            switch (g_hw_stat.av_valid[g_hw_stat.is_av_in]) {
            case 0: // video loss
                if (h_lock || vh_lock) {
                    g_hw_stat.av_valid[g_hw_stat.is_av_in] = 1;
                }
                break;

            case 1: // search
                if (vloss) {
                    det2_cnt = 0;
                    g_hw_stat.av_valid[g_hw_stat.is_av_in] = 0;
                } else if (vh_lock) {
                    det2_cnt++;
                    if (det2_cnt >= AV_DET_LOCK_CNT) {
                        det2_cnt = 0;
                        g_hw_stat.av_valid[g_hw_stat.is_av_in] = 2;
                        // ret = 1;
                    }
                } else
                    det2_cnt = 0;
                break;

            case 2: // locked
                if (vloss) {
                    det2_cnt = 0;
                    g_hw_stat.av_valid[g_hw_stat.is_av_in] = 0;
                } else if (vh_lock == 0) {
                    det2_cnt++;
                    if (det2_cnt >= AV_DET_DROP_CNT) {
                        det2_cnt = 0;
                        g_hw_stat.av_valid[g_hw_stat.is_av_in] = 1;
                    }
                } else
                    det2_cnt = 0;
                break;
            }
        }
    } else {
        // Neither scanning nor watching an analog picture, so whatever the
        // decoder is pointed at now, the next scan settles from scratch.
        settle = AV_SCAN_SETTLE;
    }

    pthread_mutex_unlock(&hardware_mutex);
    return ret;
}

// HDMI_in_detect() sets the display up only when the input timing changes, so
// coming back to HDMI in with the same signal still there (from the menu, or
// from another source) found nothing changed and left the panel off. Entering
// the source forgets what was seen, so the next detect sets it up again.
static bool hdmi_in_redetect = false;

// Counts the setups HDMI_in_detect() has done, for a caller waiting for one.
static volatile int hdmi_in_setups = 0;

int HDMI_in_setup_count(void) {
    return hdmi_in_setups;
}

void Source_HDMI_in() {
    pthread_mutex_lock(&hardware_mutex);
    hdmi_in_redetect = true;
    screen.display(0);
    I2C_Write(ADDR_FPGA, 0x8C, 0x00);

    HDZero_Close();
    // OLED_SetTMG(0);

    I2C_Write(ADDR_FPGA, 0x84, 0x00);

    g_hw_stat.source_mode = SOURCE_MODE_HDMIIN;
    Display_VO_SWITCH(1);

    pthread_mutex_unlock(&hardware_mutex);
}

// HDMI_in_detect() reads the input only in the menu and in HDMI in, so from
// any other source g_hw_stat.hdmiin_valid is whatever it saw last: the source
// cycle went to a black HDMI in after the Mac had been unplugged. One read,
// for a decision made now.
bool HDMI_in_signal_now(void) {
    pthread_mutex_lock(&hardware_mutex);
    g_hw_stat.hdmiin_valid = IT66021_Sig_det();
    bool valid = g_hw_stat.hdmiin_valid;
    pthread_mutex_unlock(&hardware_mutex);

    return valid;
}

int HDMI_in_detect() {
    static int vtmg_last = -1;
    static int cs_last = -1;
    static int last_vld = 0;
    int vtmg, cs, freq_ref;
    int ret = 0;

    pthread_mutex_lock(&hardware_mutex);

    if ((g_hw_stat.source_mode == SOURCE_MODE_UI) || (g_hw_stat.source_mode == SOURCE_MODE_HDMIIN)) {

        last_vld = g_hw_stat.hdmiin_valid;
        g_hw_stat.hdmiin_valid = IT66021_Sig_det();

        if (g_hw_stat.source_mode == SOURCE_MODE_HDMIIN) {
            if (hdmi_in_redetect) {
                hdmi_in_redetect = false;
                vtmg_last = -1;
                cs_last = -1;
            }
            if (g_hw_stat.hdmiin_valid) {
                vtmg = IT66021_Get_VTMG(&freq_ref);
                if (vtmg_last != vtmg) {
                    vtmg_last = vtmg;
                    ret = 1;
                    LOGI("IT66021: VTMG change: %d", vtmg);

                    screen.display(0);
                    I2C_Write(ADDR_FPGA, 0x8C, 0x00);

                    switch (vtmg) {
                    case HDMIIN_VTMG_UNKNOW:
                        break;

                    case HDMIIN_VTMG_1080P60:
                        system_exec("dispw -s vdpo 1080p60");
                        dvr_update_vi_conf(VR_1080P60);
                        g_hw_stat.vdpo_tmg = VDPO_TMG_1080P60;
                        vclk_phase_set(VIDEO_SOURCE_HDMI_IN_1080P60, (freq_ref < 63));
                        pclk_phase_set(VIDEO_SOURCE_HDMI_IN_1080P60);
                        I2C_Write(ADDR_FPGA, 0x80, 0x00);

                        screen.vtmg(0);
                        I2C_Write(ADDR_FPGA, 0xa7, 0x00);

                        I2C_Write(ADDR_FPGA, 0x8C, 0x04);
                        I2C_Write(ADDR_FPGA, 0x06, 0x0F);
                        g_hw_stat.hdmiin_vtmg = HDMIIN_VTMG_1080P60;
                        break;

                    case HDMIIN_VTMG_1080P50:
                        system_exec("dispw -s vdpo 1080p50");
                        dvr_update_vi_conf(VR_1080P50);
                        g_hw_stat.vdpo_tmg = VDPO_TMG_1080P50;
                        vclk_phase_set(VIDEO_SOURCE_HDMI_IN_1080P50, (freq_ref < 63));
                        pclk_phase_set(VIDEO_SOURCE_HDMI_IN_1080P50);

                        I2C_Write(ADDR_FPGA, 0x80, 0x20);

                        screen.vtmg(0);
                        I2C_Write(ADDR_FPGA, 0xa7, 0x00);

                        I2C_Write(ADDR_FPGA, 0x8C, 0x04);
                        I2C_Write(ADDR_FPGA, 0x06, 0x0F);
                        g_hw_stat.hdmiin_vtmg = HDMIIN_VTMG_1080P50;
                        break;

                    case HDMIIN_VTMG_1080Pother:
                        system_exec("dispw -s vdpo 1080p60");
                        dvr_update_vi_conf(VR_1080P60);
                        g_hw_stat.vdpo_tmg = VDPO_TMG_1080P60;
                        vclk_phase_set(VIDEO_SOURCE_HDMI_IN_1080P60, (freq_ref < 63));
                        pclk_phase_set(VIDEO_SOURCE_HDMI_IN_1080P60);

                        I2C_Write(ADDR_FPGA, 0x80, 0x40);

                        screen.vtmg(0);
                        I2C_Write(ADDR_FPGA, 0xa7, 0x00);

                        I2C_Write(ADDR_FPGA, 0x8C, 0x04);
                        I2C_Write(ADDR_FPGA, 0x06, 0x0F);
                        g_hw_stat.hdmiin_vtmg = HDMIIN_VTMG_1080Pother;
                        break;

                    case HDMIIN_VTMG_720P50:
                        system_exec("dispw -s vdpo 720p50");
                        dvr_update_vi_conf(VR_720P50);
                        g_hw_stat.vdpo_tmg = VDPO_TMG_720P50;
                        vclk_phase_set(VIDEO_SOURCE_HDMI_IN_720P50, (freq_ref < 63));
                        pclk_phase_set(VIDEO_SOURCE_HDMI_IN_720P50);

                        I2C_Write(ADDR_FPGA, 0x80, 0x60);

                        screen.vtmg(1);
                        I2C_Write(ADDR_FPGA, 0xa7, 0x11);

                        I2C_Write(ADDR_FPGA, 0x8C, 0x04);
                        I2C_Write(ADDR_FPGA, 0x06, 0x0F);
                        g_hw_stat.hdmiin_vtmg = HDMIIN_VTMG_720P50;
                        break;

                    case HDMIIN_VTMG_720P60:
                        system_exec("dispw -s vdpo 720p60");
                        dvr_update_vi_conf(VR_720P60);
                        g_hw_stat.vdpo_tmg = VDPO_TMG_720P60;
                        vclk_phase_set(VIDEO_SOURCE_HDMI_IN_720P60, (freq_ref < 63));
                        pclk_phase_set(VIDEO_SOURCE_HDMI_IN_720P60);
                        I2C_Write(ADDR_FPGA, 0x80, 0x80);

                        screen.vtmg(1);
                        I2C_Write(ADDR_FPGA, 0xa7, 0x11);

                        I2C_Write(ADDR_FPGA, 0x8C, 0x04);
                        I2C_Write(ADDR_FPGA, 0x06, 0x0F);
                        g_hw_stat.hdmiin_vtmg = HDMIIN_VTMG_720P60;
                        break;

                    case HDMIIN_VTMG_720P100:
                        system_exec("dispw -s vdpo 720p30"); // 100fps actually
                        dvr_update_vi_conf(VR_540P90);
                        g_hw_stat.vdpo_tmg = VDPO_TMG_720P100;
                        vclk_phase_set(VIDEO_SOURCE_HDMI_IN_720P100, (freq_ref < 63));
                        pclk_phase_set(VIDEO_SOURCE_HDMI_IN_720P100);

                        I2C_Write(ADDR_FPGA, 0x80, 0xA0);

                        screen.vtmg(1);
                        I2C_Write(ADDR_FPGA, 0xa7, 0x01);

                        I2C_Write(ADDR_FPGA, 0x8C, 0x04);
                        I2C_Write(ADDR_FPGA, 0x06, 0x0F);
                        g_hw_stat.hdmiin_vtmg = HDMIIN_VTMG_720P100;
                        break;
                    }

                    if (vtmg != HDMIIN_VTMG_UNKNOW)
                        screen.display(1);
                    hdmi_in_setups++;
                }

                cs = IT66021_Get_CS();
                if (cs_last != cs) {
                    cs_last = cs;
                    LOGI("IT66021: Color space change: %d\n", cs);

                    IT66021_Set_CSMatrix(cs);
                }
            } else {
                if (last_vld)
                    I2C_Write(ADDR_FPGA, 0x06, 0x0F);

                vtmg_last = -1;
                cs_last = -1;
            }
        } else {
            vtmg_last = -1;
            cs_last = -1;
        }
    }
    pthread_mutex_unlock(&hardware_mutex);

    return ret;
}

void Display_Osd(bool enable) {
    I2C_Write(ADDR_FPGA, 0x84, enable ? 0x11 : 0x01);
}

// The goggle's whole layer over analog video, for the OSD toggle. Bit 0 lays
// the SoC's output over the picture and 0x00 is how HDMI in runs, with no
// layer at all; bit 4 goes with record.osd as in Display_Osd(). The layer
// carries the FC OSD grid and anything else the app draws, which the element
// by element hiding in osd_hdzero_update() does not all reach.
void Display_Osd_shown(bool shown, bool record) {
    I2C_Write(ADDR_FPGA, 0x84, shown ? (record ? 0x11 : 0x01) : 0x00);
}

void Set_Brightness(uint8_t bri) {
    int8_t val = 0x80 + bri - 39;
    I2C_Write(ADDR_FPGA, 0x85, val);
}

void Set_Contrast(uint8_t con) {
    int8_t val = con << 2;
    I2C_Write(ADDR_FPGA, 0x86, val);
}

void Set_Saturation(uint8_t sat) {
    int8_t val = sat << 2;
    I2C_Write(ADDR_FPGA, 0x87, val);
}

void Set_HT_status(uint8_t is_open, uint8_t frame_period, uint8_t sync_len) // frame_period unit = 2ms
{
    I2C_Write(ADDR_FPGA, 0x71, sync_len);
    I2C_Write(ADDR_FPGA, 0x70, (frame_period << 4) | is_open);
}

void Set_HT_dat(uint16_t ch0, uint16_t ch1, uint16_t ch2) {
    I2C_Write(ADDR_FPGA, 0x72, ch0 & 0xff);
    I2C_Write(ADDR_FPGA, 0x73, (ch0 >> 8) & 0xff);
    I2C_Write(ADDR_FPGA, 0x74, ch1 & 0xff);
    I2C_Write(ADDR_FPGA, 0x75, (ch1 >> 8) & 0xff);
    I2C_Write(ADDR_FPGA, 0x76, ch2 & 0xff);
    I2C_Write(ADDR_FPGA, 0x77, (ch2 >> 8) & 0xff);
}

void Analog_Module_Power(bool ForceSet) {
    // Batch 2 goggles only
    if (getHwRevision() >= HW_REV_2) {
        static bool Analog_Module_Power_State = 0;
        static bool Analog_Module_Power_State_Last = 0;
        if (g_setting.power.power_ana == 0) {
            Analog_Module_Power_State = 0;
        } else {
            if (g_source_info.source != SOURCE_AV_MODULE) {
                Analog_Module_Power_State = 1;
            } else {
                Analog_Module_Power_State = 0;
            }
        }
        if ((Analog_Module_Power_State_Last != Analog_Module_Power_State) || (ForceSet == 1)) {
            Analog_Module_Power_State_Last = Analog_Module_Power_State;
            DM5680_ExternalAnalog_Power(Analog_Module_Power_State);
        }
    }
}

int Get_VideoLatancy_status() // ret: 0=unlocked, 1=locked
{
    uint8_t rdat = 0;

    if (g_hw_stat.source_mode == SOURCE_MODE_AV || g_hw_stat.source_mode == SOURCE_MODE_HDZERO) {
        rdat = I2C_Read(ADDR_FPGA, 0x11);
        rdat = (rdat < 0x04) ? 1 : 0;
    }

    return rdat;
}

int Get_HAN_status() // ret: 0=error; 1=ok
{
    uint8_t r18 = 0;
    uint8_t r19 = 0;

    if (fpga_old_clocks()) {
        // 9.5.1's check, for its FPGA: 9.6's reads 0x19 as well and expects
        // values only its own FPGA gives, so the self test failed here.
        I2C_Write(ADDR_FPGA, 0x81, 0x01);
        usleep(10000);
        r18 = I2C_Read(ADDR_FPGA, 0x18);
        I2C_Write(ADDR_FPGA, 0x81, 0x00);
        return r18 >= 0xE4 && r18 <= 0xE7;
    }

    I2C_Write(ADDR_FPGA, 0x81, 0x01);
    sleep(1);

    r18 = I2C_Read(ADDR_FPGA, 0x18);
    r19 = I2C_Read(ADDR_FPGA, 0x19);

    if (r19 == 0x1C && r18 >= 0xF0 && r18 <= 0xF6)
        r19 = 1;
    else
        r19 = 0;

    I2C_Write(ADDR_FPGA, 0x81, 0x00);

    return r19;
}

#endif
