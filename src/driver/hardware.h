#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "ui/page_common.h"

typedef enum {
    SOURCE_MODE_UI = 0,
    SOURCE_MODE_HDZERO = 1,
    SOURCE_MODE_AV = 2,
    SOURCE_MODE_HDMIIN = 3,
} source_mode_t;

typedef enum {
    HDMIIN_VTMG_UNKNOW = 0,
    HDMIIN_VTMG_1080P60 = 1,
    HDMIIN_VTMG_1080P50 = 2,
    HDMIIN_VTMG_1080Pother = 3,
    HDMIIN_VTMG_720P50 = 4,
    HDMIIN_VTMG_720P60 = 5,
    HDMIIN_VTMG_720P100 = 6,
} hdmiin_vtmg_t;

typedef enum {
    VDPO_TMG_720P50 = 0,
    VDPO_TMG_720P60 = 1,
    VDPO_TMG_720P90 = 2,
    VDPO_TMG_1080P50 = 3,
    VDPO_TMG_720P100 = 4,
    VDPO_TMG_1080P60 = 5,
} vdpo_tmg_t;

typedef enum {
    VIDEO_SOURCE_VERSION = 0,
    VIDEO_SOURCE_MENU_UI = 1,
    VIDEO_SOURCE_HDZERO_IN_720P60_50 = 2,
    VIDEO_SOURCE_HDZERO_IN_720P90 = 3,
    VIDEO_SOURCE_HDZERO_IN_1080P30 = 4,
    VIDEO_SOURCE_AV_IN = 5,
    VIDEO_SOURCE_HDMI_IN_1080P50 = 6,
    VIDEO_SOURCE_HDMI_IN_1080P60 = 7,
    VIDEO_SOURCE_HDMI_IN_1080POTHER = 8,
    VIDEO_SOURCE_HDMI_IN_720P50 = 9,
    VIDEO_SOURCE_HDMI_IN_720P60 = 10,
    VIDEO_SOURCE_HDMI_IN_720P100 = 11,
    VIDEO_SOURCE_HDMI_OUT = 11,
    VIDEO_SOURCE_TP2825_EX = 12,
    VIDEO_SOURCE_NUM = 13,
} video_source_t;

typedef struct {
    source_mode_t source_mode;
    vdpo_tmg_t vdpo_tmg;

    // hdzero
    int hdz_bw; // 0=27MHz; 1=17MHz
    int hdzero_open;
    int hdz_standby; // 1=baseband stopped but DM6302 still configured
    int m0_open;

    // av in
    int is_av_in;
    int av_pal[2];
    int av_pal_w;
    int av_valid[2]; // 0=AV in; 1=RF Analog

    // hdmi in
    int hdmiin_valid;
    hdmiin_vtmg_t hdmiin_vtmg;

    int IS_TP2825_L;

} hw_status_t;

extern hw_status_t g_hw_stat;
extern int fhd_req;

void hw_stat_init();

void hw_screen_on(int bON);
// 0 = UI, 1 = the live source. Reopens the M0 when the tuner is open.
void Display_VO_SWITCH(uint8_t sel);
void HDZero_open(int bw);
void HDZero_Close();
void HDZero_Standby();
// Run HDZero_open(bw) on a worker. The next HDZero_open/Close/Standby waits
// for it first, so callers need not know whether it is still running.
void HDZero_open_async_start(int bw);
// True while that worker has not been collected yet.
bool HDZero_open_pending(void);
// Wait for an outstanding async tuner init to finish. For start-up code on the
// main thread that shares the I2C bus with it and cannot run at the 1MHz the
// init sets; see the comment on the definition.
void HDZero_open_async_wait(void);

void Source_HDMI_in();
void Source_AV(bool is_av_in);
void Display_UI_init();
// True once dispw has actually been run, so callers can tell a real display
// state from the assumed boot one.
bool vdpo_timing_applied(void);
// True while a background timing change has been started and not yet
// collected by vdpo_set_timing(). This outlasts dispw itself; for "is the
// display still being reconfigured right now", use vdpo_timing_running().
bool vdpo_timing_pending(void);
// True only while the dispw process is actually running.
bool vdpo_timing_running(void);
// Finish any outstanding background timing change. A no-op when there is
// none; there so no path can leave one hanging.
void vdpo_timing_collect(void);
// Begin a display timing change in the background; the next vdpo_set_timing()
// for the same timing collects it instead of running dispw itself.
void vdpo_start_timing_async(vdpo_tmg_t tmg, const char *mode);
void Display_UI();

void Display_720P90(int mode);
void Display_720P60_50(int mode, uint8_t is_43);
void Display_1080P30(int mode);
void Display_1080P24(int mode);

void Display_HDZ(int mode, int is_43);
void Display_Osd(bool enable);

void Set_Brightness(uint8_t bri);
void Set_Contrast(uint8_t con);
void Set_Saturation(uint8_t sat);

void Set_HT_status(uint8_t is_open, uint8_t frame_period, uint8_t sync_len);
void Set_HT_dat(uint16_t ch0, uint16_t ch1, uint16_t ch2);

void Analog_Module_Power(bool Force);

int HDZERO_detect();
int AV_in_detect();
int HDMI_in_detect();

int Get_VideoLatancy_status(); // ret: 0=unlocked, 1=locked
int Get_HAN_status();          // ret: 0=error; 1=ok

void vclk_phase_init();
void pclk_phase_init();

extern uint32_t vclk_phase[VIDEO_SOURCE_NUM];

#ifdef __cplusplus
}
#endif
