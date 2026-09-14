#pragma once

#define DIAL_SENSITIVITY           1    // number of clicks before dial event is triggered
#define DIAL_SENSITIVTY_TIMEOUT_MS 1000 // ms
#define CHANNEL_SHOWTIME           30   // must <= 127

#define GPIO_BEEP 131

#define GPIO_ESP32_BOOT0 128
#define GPIO_ESP32_EN    129
#define GPIO_ESP32_BOOT  130

#define GPIO_TP2825_RSTB 132
#define GPIO_TP2825_PDN  133
#define GPIO_RTC6715_ON  134
#define GPIO_IS_PRO      136

#define GPIO_HDZ_RX_RESET 224
#define GPIO_HDZ_TX_RESET 228
#define GPIO_FPGA_RESET   258

#define DEV_SPI_VTX_VRX_R "/dev/mtd8"
#define DEV_SPI_VRX_L     "/dev/mtd9"
#define DEV_SPI_VA        "/dev/mtd10"

#define SELF_TEST_FILE    "/mnt/extsd/self_test.txt"
#define NO_DIAL_FILE      "/mnt/extsd/no_dial.txt"
#define APP_LOG_FILE      "/mnt/extsd/HDZGOGGLE.log"
#define APP_LOG_FILE_PREV "/mnt/extsd/HDZGOGGLE.prev.log"
// Finished boots live here, one file each, numbered upwards: the current one
// is APP_LOG_FILE in the root, where rc.sh expects it, and everything else is
// out of the way of the recordings.
#define APP_LOG_DIR       "/mnt/extsd/boot-logs"
#define APP_LOG_FILE_OLD  APP_LOG_DIR "/HDZGOGGLE.%04u.log"
// The record process's own output. rc.sh sends it here whenever the file
// exists at boot; it is truncated every boot, so the app moves it in with
// the boot logs under the number this boot's HDZGOGGLE.log will get.
#define REC_LOG_FILE      "/mnt/extsd/RECORD.log"
#define REC_LOG_FILE_OLD  APP_LOG_DIR "/RECORD.%04u.log"
// How many boots to keep. Two -- this boot and the one before -- meant a log
// was one power-on away from being gone, and the boot worth reading is always
// identified after the fact. At about 100KB a boot, 999 of them is under
// 100MB: 35 seconds of video on a card that holds six hours, at the measured
// 2.86MB/s. The number in the name keeps climbing; only the window moves.
#define APP_LOG_KEEP      999
#define APP_BIN_FILE      "/mnt/extsd/HDZGOGGLE"
#define DEVELOP_SCRIPT    "/mnt/extsd/develop.sh"// The 9.5.1 Goggle 2 tree had this as a runtime check; in the merged tree the
// target is a compile-time choice, so it is one here too.
#ifdef HDZGOGGLE2
#define GOGGLE_VER_2 1
#else
#define GOGGLE_VER_2 0
#endif


