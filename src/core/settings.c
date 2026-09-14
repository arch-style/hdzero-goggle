#include "settings.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <log/log.h>
#include <minIni.h>

#include "../conf/targets.h"

#include "core/self_test.h"
#include "lang/language.h"
#include "driver/dm6302.h"
#include "ui/page_common.h"
#include "ui/page_scannow.h"
#include "util/filesystem.h"
#include "util/system.h"
#include "util/time.h"

#define SETTINGS_INI_VERSION_UNKNOWN 0

setting_t g_setting;

// Slowest first, so an index that was saved before a value was added still
// means the same time.
const uint16_t long_press_choices[LONG_PRESS_CHOICE_NUM] = {500, 400, 300, 200, 100, 50};

int long_press_choice_index(uint16_t ms) {
    for (int i = 0; i < LONG_PRESS_CHOICE_NUM; i++) {
        if (long_press_choices[i] == ms)
            return i;
    }

    return -1;
}

const setting_t g_setting_defaults = {
    .scan = {
        .channel = 1,
    },
    .favorites = {
        .hdzero = {
            .enable = false,
            .count = 4,
            .channel = {0, 0, 0, 0, 0, 0, 0, 0},
        },
        .analog = {
            .enable = false,
            .count = 4,
            .channel = {0, 0, 0, 0, 0, 0, 0, 0},
        },
    },
    .fans = {
        .top_speed = 4,
        .auto_mode = true,
        .left_speed = 5,
        .right_speed = 5,
    },
    .autoscan = {
        .status = SETTING_AUTOSCAN_STATUS_ON,
        .last_source = SETTING_AUTOSCAN_SOURCE_LAST,
        .source = SETTING_AUTOSCAN_SOURCE_HDZERO,
    },
    .power = {
        .voltage = 3500,
        .display_voltage = true,
        .warning_type = SETTING_POWER_WARNING_TYPE_BOTH,
        .cell_count_mode = SETTING_POWER_CELL_COUNT_MODE_AUTO,
        .cell_count = 2,
        .osd_display_mode = SETTING_POWER_OSD_DISPLAY_MODE_TOTAL,
        .power_ana = false,
        .calibration_offset = 0,
    },
    .record = {
        .mode_manual = false,
        .format_ts = true,
        .bitrate_scale = SETTING_RECORD_BITRATE_SCALE_NORMAL,
        .osd = true,
        .audio = true,
        .audio_source = SETTING_RECORD_AUDIO_SOURCE_MIC,
        .naming = SETTING_NAMING_CONTIGUOUS,
    },
    .image = {
#if defined(HDZGOGGLE) || defined(HDZGOGGLE2)
        .oled = 8,
        .saturation = 28,
        .contrast = 25,
#elif defined(HDZBOXPRO)
        .oled = 12,
        .saturation = 47,
        .contrast = 30,
#endif
        .brightness = 39,
        .auto_off = 1,
    },
    .ht = {
        .enable = false,
        .max_angle = 120,
        .acc_x = 0,
        .acc_y = 0,
        .acc_z = 0,
        .gyr_x = 0,
        .gyr_y = 0,
        .gyr_z = 0,
        .alarm_state = SETTING_HT_ALARM_STATE_OFF,
        .alarm_angle = 1300,
        .alarm_delay = 5,
        .alarm_pattern = SETTING_HT_ALARM_PATTERN_2SHORT,
        .alarm_on_arm = false,
        .alarm_on_video = false,
    },
    .elrs = {
        .enable = false,
    },
    .ease = {
        .no_dial = 0,
    },
    .speed = {
        .fast_menu = false,
        .keep_display = false,
        .skip_audio = false,
        .boot_display = false,
        .boot_fonts = false,
        .skip_boot_menu = false,
        .async_imu = false,
        .async_display = false,
        .ui_throttle = false,
        .label_diff = false,
        .timed_long_press = false,
        .split_lock = false,
        .menu_antialias_off = false,
        .async_tuner = false,
        .spi_burst = false,
        .tuner_bus = 0,
        .tuner_bus_duty40 = false,
        .skip_wifi_stop = false,
        .boot_display_early = false,
        .defer_menu = false,
        .fast_efuse = false,
        .dvr_stop_wait = false,
        .dvr_start_wait = false,
        .menu_async_display = false,
        .dvr_defer_stop = false,
    },
    .bugfix = {
        .retry_tuner_init = false,
        .wait_for_recording = false,
        .short_i2c_timeout = false,
        .drop_queued_presses = false,
        .dvr_give_up = false,
    },
    .input = {
        .button_beep = false,
        .dial_beep = false,
        .long_press_ms = 500,
    },
    .osd = {
        .orbit = 2,
        .embedded_mode = EMBEDDED_4x3,
        .startup_visibility = SETTING_OSD_SHOW_AT_STARTUP_SHOW,
        .is_visible = true,
        .element = {
            // OSD_GOGGLE_TOPFAN_SPEED
            {
                .show = true,
                .position = {.mode_4_3 = {.x = 160, .y = 0}, .mode_16_9 = {.x = 0, .y = 0}},
            },
            // OSD_GOGGLE_LATENCY_LOCK
            {
                .show = true,
                .position = {.mode_4_3 = {.x = 200, .y = 0}, .mode_16_9 = {.x = 40, .y = 0}},
            },
            // OSD_GOGGLE_VTX_TEMP
            {
                .show = true,
                .position = {.mode_4_3 = {.x = 240, .y = 0}, .mode_16_9 = {.x = 80, .y = 0}},
            },
            // OSD_GOGGLE_VRX_TEMP
            {
                .show = true,
                .position = {.mode_4_3 = {.x = 280, .y = 0}, .mode_16_9 = {.x = 120, .y = 0}},
            },
            // OSD_GOGGLE_BATTERY_LOW
            {
                .show = true,
                .position = {.mode_4_3 = {.x = 320, .y = 0}, .mode_16_9 = {.x = 160, .y = 0}},
            },
            // OSD_GOGGLE_BATTERY_VOLTAGE
            {
                .show = true,
                .position = {.mode_4_3 = {.x = 360, .y = 0}, .mode_16_9 = {.x = 200, .y = 0}},
            },
            // OSD_GOGGLE_CLOCK_DATE
            {
                .show = false,
                .position = {.mode_4_3 = {.x = 360, .y = 24}, .mode_16_9 = {.x = 200, .y = 24}},
            },
            // OSD_GOGGLE_CLOCK_TIME
            {
                .show = false,
                .position = {.mode_4_3 = {.x = 580, .y = 24}, .mode_16_9 = {.x = 420, .y = 24}},
            },
            // OSD_GOGGLE_CHANNEL
            {
                .show = true,
                .position = {.mode_4_3 = {.x = 580, .y = 0}, .mode_16_9 = {.x = 580, .y = 0}},
            },
            // OSD_GOGGLE_SD_REC
            {
                .show = true,
                .position = {.mode_4_3 = {.x = 840, .y = 0}, .mode_16_9 = {.x = 1000, .y = 0}},
            },
            // OSD_GOGGLE_VLQ
            {
                .show = true,
                .position = {.mode_4_3 = {.x = 880, .y = 0}, .mode_16_9 = {.x = 1040, .y = 0}},
            },
            // OSD_GOGGLE_ANT0
            {
                .show = true,
                .position = {.mode_4_3 = {.x = 960, .y = 0}, .mode_16_9 = {.x = 1120, .y = 0}},
            },
            // OSD_GOGGLE_ANT1
            {
                .show = true,
                .position = {.mode_4_3 = {.x = 920, .y = 0}, .mode_16_9 = {.x = 1080, .y = 0}},
            },
            // OSD_GOGGLE_ANT2
            {
                .show = true,
                .position = {.mode_4_3 = {.x = 1040, .y = 0}, .mode_16_9 = {.x = 1200, .y = 0}},
            },
            // OSD_GOGGLE_ANT3
            {
                .show = true,
                .position = {.mode_4_3 = {.x = 1000, .y = 0}, .mode_16_9 = {.x = 1160, .y = 0}},
            },
            // OSD_GOGGLE_TEMP_TOP
            {
                .show = true,
                .position = {.mode_4_3 = {.x = 170, .y = 50}, .mode_16_9 = {.x = 170, .y = 50}},
            },
            // OSD_GOGGLE_TEMP_LEFT
            {
                .show = true,
                .position = {.mode_4_3 = {.x = 270, .y = 50}, .mode_16_9 = {.x = 270, .y = 50}},
            },
            // OSD_GOGGLE_TEMP_RIGHT
            {
                .show = true,
                .position = {.mode_4_3 = {.x = 370, .y = 50}, .mode_16_9 = {.x = 370, .y = 50}},
            },
        },
    },
    .clock = {
        .year = 2023,
        .month = 3,
        .day = 28,
        .hour = 12,
        .min = 30,
        .sec = 30,
        .format = 0,
    },
    // Refer to `page_input.c`'s arrays `rollerFunctionPointers` and `btnFunctionPointers`
    .inputs = {
        .roller = 0,
        .left_click = 0,
        .left_press = 1,
        .right_click = 2,
        .right_press = 6,
        .right_double_click = 3,
    },
    .wifi = {
        .enable = false,
        .mode = 0,
        .clientid = {""},
        .ssid = {"HDZero", "MySSID"},
        .passwd = {"divimath", "MyPassword"},
        .dhcp = true,
        .ip_addr = "192.168.2.122",
        .netmask = "255.255.255.0",
        .gateway = "192.168.2.1",
        .dns = "192.168.2.1",
        .rf_channel = 11,
        .root_pw = "divimath",
        .ssh = false,
    },
    .storage = {
        .logging = false,
        .selftest = false,
    },
    .source = {
        .analog_channel = 33, // R1
        .analog_format = SETTING_SOURCES_ANALOG_FORMAT_NTSC,
        .analog_ratio = SETTING_SOURCES_ANALOG_RATIO_4_3,
        .hdzero_band = SETTING_SOURCES_HDZERO_BAND_RACEBAND,
        .hdzero_bw = SETTING_SOURCES_HDZERO_BW_WIDE,
    },
    .language = {
        .lang = LANG_ENGLISH_DEFAULT,
    },
    .analog_rssi = {
        .calib_min = 1600,
        .calib_max = 2100,
    },
    .has_all_features = true,
};

// channel_max is the width of the name table channel2str() indexes for this
// list, not what the current band happens to offer: a slot registered on
// Raceband is still a legal 12 while Lowband is selected, and slot_usable()
// is what decides whether it can be tuned to. Anything past the table is a
// hand-edited setting.ini, and channel2str() indexes straight into the table,
// so it has to be turned into an empty slot here rather than trusted.
static void settings_load_favorites(setting_favorites_list_t *list, const setting_favorites_list_t *defaults,
                                    const char *section, uint8_t channel_max) {
    list->enable = settings_get_bool((char *)section, "enable", defaults->enable);

    list->count = ini_getl(section, "count", defaults->count, SETTING_INI);
    if ((list->count < 1) || (list->count > FAVORITES_MAX))
        list->count = defaults->count;

    for (int i = 0; i < FAVORITES_MAX; i++) {
        char key[8];
        long ch;

        snprintf(key, sizeof(key), "ch%d", i + 1);
        ch = ini_getl(section, key, defaults->channel[i], SETTING_INI);

        // 0 is the empty slot and is always legal.
        if ((ch < 0) || (ch > channel_max)) {
            LOGW("%s: %s=%ld is out of range, slot left empty", section, key, ch);
            ch = 0;
        }

        list->channel[i] = (uint8_t)ch;
    }
}

int settings_put_osd_element_shown(bool show, char *config_name) {
    char setting_key[128];

    snprintf(setting_key, sizeof(setting_key), "element_%s_show", config_name);
    return settings_put_bool("osd", setting_key, show);
}

int settings_put_osd_element_pos_x(const setting_osd_goggle_element_positions_t *pos, char *config_name) {
    char setting_key[128];
    int ret = 0;

    snprintf(setting_key, sizeof(setting_key), "element_%s_pos_4_3_x", config_name);
    ret = ini_putl("osd", setting_key, pos->mode_4_3.x, SETTING_INI);
    snprintf(setting_key, sizeof(setting_key), "element_%s_pos_16_9_x", config_name);
    ret &= ini_putl("osd", setting_key, pos->mode_16_9.x, SETTING_INI);
    return ret;
}

int settings_put_osd_element_pos_y(const setting_osd_goggle_element_positions_t *pos, char *config_name) {
    char setting_key[128];
    int ret = 0;

    snprintf(setting_key, sizeof(setting_key), "element_%s_pos_4_3_y", config_name);
    ret = ini_putl("osd", setting_key, pos->mode_4_3.y, SETTING_INI);
    snprintf(setting_key, sizeof(setting_key), "element_%s_pos_16_9_y", config_name);
    ret &= ini_putl("osd", setting_key, pos->mode_16_9.y, SETTING_INI);
    return ret;
}

int settings_put_osd_element(const setting_osd_goggle_element_t *element, char *config_name) {
    int ret = 0;

    ret = settings_put_osd_element_shown(element->show, config_name);
    ret &= settings_put_osd_element_pos_x(&element->position, config_name);
    ret &= settings_put_osd_element_pos_y(&element->position, config_name);
    return ret;
}

static void settings_load_osd_element(setting_osd_goggle_element_t *element, char *config_name, const setting_osd_goggle_element_t *defaults) {
    char buf[128];

    snprintf(buf, sizeof(buf), "element_%s_show", config_name);
    element->show = settings_get_bool("osd", buf, defaults->show);

    snprintf(buf, sizeof(buf), "element_%s_pos_4_3_x", config_name);
    element->position.mode_4_3.x = ini_getl("osd", buf, defaults->position.mode_4_3.x, SETTING_INI);

    snprintf(buf, sizeof(buf), "element_%s_pos_4_3_y", config_name);
    element->position.mode_4_3.y = ini_getl("osd", buf, defaults->position.mode_4_3.y, SETTING_INI);

    snprintf(buf, sizeof(buf), "element_%s_pos_16_9_x", config_name);
    element->position.mode_16_9.x = ini_getl("osd", buf, defaults->position.mode_16_9.x, SETTING_INI);

    snprintf(buf, sizeof(buf), "element_%s_pos_16_9_y", config_name);
    element->position.mode_16_9.y = ini_getl("osd", buf, defaults->position.mode_16_9.y, SETTING_INI);
}

bool settings_get_bool(char *section, char *key, bool default_val) {
    char buf[128];

    ini_gets(section, key, default_val ? "true" : "false", buf, sizeof(buf), SETTING_INI);
    return strcmp(buf, "true") == 0;
}

int settings_put_bool(char *section, char *key, bool value) {
    return ini_puts(section, key, value ? "true" : "false", SETTING_INI);
}

void settings_reset(void) {
    char buf[256];

    snprintf(buf, sizeof(buf), "rm -f %s", SETTING_INI);
    system_exec(buf);
    usleep(50);

    snprintf(buf, sizeof(buf), "touch %s", SETTING_INI);
    system_exec(buf);
    usleep(50);

    ini_putl("settings", "file_version", SETTING_INI_VERSION, SETTING_INI);
}

void settings_init(void) {
    // check if backup of old settings file exists after goggle update
    if (fs_file_exists("/mnt/UDISK/setting.ini")) {
        char buf[256];
        snprintf(buf, sizeof(buf), "cp -f /mnt/UDISK/setting.ini %s", SETTING_INI);
        system_exec(buf);
        usleep(10);
        system_exec("rm /mnt/UDISK/setting.ini");
    }

    int file_version = ini_getl("settings", "file_version", SETTINGS_INI_VERSION_UNKNOWN, SETTING_INI);
    if (file_version != SETTING_INI_VERSION)
        settings_reset();
}

// One file per finished boot in APP_LOG_DIR, numbered upwards, and the number
// never goes back: the newest boot takes the highest number seen plus one, and
// anything more than APP_LOG_KEEP behind it is deleted. Nothing is renamed to
// make room, which is the point -- a scheme that shifts .1 to .2 and so on
// costs a rename per kept boot at every start-up, and this branch exists to
// keep things off the boot path.
//
// Reads the sequence number back out of the file names rather than storing it
// anywhere. A card carrying its own history needs no state on the goggles,
// works when the card is swapped, and cannot disagree with what is there.
static bool app_log_seq(const char *name, unsigned *seq) {
    char tail;

    // The %c is how the end of the string is checked: it must not match, so
    // exactly one conversion is the whole name and nothing after it.
    return sscanf(name, "HDZGOGGLE.%u.log%c", seq, &tail) == 1;
}

static bool rec_log_seq(const char *name, unsigned *seq) {
    char tail;

    return sscanf(name, "RECORD.%u.log%c", seq, &tail) == 1;
}

static unsigned app_log_newest(void) {
    unsigned newest = 0, seq;
    struct dirent *entry;
    DIR *dir = opendir(APP_LOG_DIR);

    if (!dir)
        return 0;

    while ((entry = readdir(dir)))
        if (app_log_seq(entry->d_name, &seq) && seq > newest)
            newest = seq;

    closedir(dir);
    return newest;
}

// By number rather than by counting files, so a log deleted by hand leaves a
// gap instead of keeping an older one alive. Unlinking during the walk is
// allowed to miss an entry -- the next boot sees it again.
static void app_log_prune(unsigned oldest_kept) {
    char path[160];
    unsigned seq;
    struct dirent *entry;
    DIR *dir = opendir(APP_LOG_DIR);

    if (!dir)
        return;

    while ((entry = readdir(dir))) {
        if ((app_log_seq(entry->d_name, &seq) || rec_log_seq(entry->d_name, &seq)) && seq < oldest_kept) {
            snprintf(path, sizeof(path), APP_LOG_DIR "/%s", entry->d_name);
            unlink(path);
        }
    }

    closedir(dir);
}

static void app_log_rotate(void) {
    uint32_t started_ms = time_ms();
    char path[160];
    unsigned next;

    mkdir(APP_LOG_DIR, 0777);

    next = app_log_newest() + 1;

    if (next > APP_LOG_KEEP)
        app_log_prune(next - APP_LOG_KEEP);

    // Once, on the first boot after the two-file scheme. It is the older of
    // the two, so it goes in first and keeps that order.
    if (fs_file_exists(APP_LOG_FILE_PREV)) {
        snprintf(path, sizeof(path), APP_LOG_FILE_OLD, next++);
        rename(APP_LOG_FILE_PREV, path);
    }

    snprintf(path, sizeof(path), APP_LOG_FILE_OLD, next);
    rename(APP_LOG_FILE, path);
    unlink(APP_LOG_FILE);

    // The record process is already writing this boot's RECORD.log, opened
    // by rc.sh before the app started. Renaming does not disturb that; it
    // just puts the file where the next boot will not truncate it, numbered
    // with this boot's own log. A fresh empty one keeps rc.sh redirecting.
    if (fs_file_exists(REC_LOG_FILE)) {
        snprintf(path, sizeof(path), REC_LOG_FILE_OLD, next + 1);
        if (rename(REC_LOG_FILE, path) == 0) {
            int fd = open(REC_LOG_FILE, O_WRONLY | O_CREAT, 0666);
            if (fd >= 0)
                close(fd);
        }
    }

    // Two directory walks on the card, and this is the boot path. Silent
    // unless it starts to cost something worth knowing about.
    uint32_t took_ms = time_ms() - started_ms;
    if (took_ms >= 20)
        LOGI("log: rotate to %u took %ums", next, took_ms);
}

void settings_load(void) {
    // Start with a fully configured structure then update!
    memcpy(&g_setting, &g_setting_defaults, sizeof(g_setting));

    // scan
    g_setting.scan.channel = ini_getl("scan", "channel", g_setting_defaults.scan.channel, SETTING_INI);

    // fans
    g_setting.fans.auto_mode = settings_get_bool("fans", "auto", g_setting_defaults.fans.auto_mode);
    g_setting.fans.top_speed = ini_getl("fans", "top_speed", g_setting_defaults.fans.top_speed, SETTING_INI);
    g_setting.fans.left_speed = ini_getl("fans", "left_speed", g_setting_defaults.fans.left_speed, SETTING_INI);
    g_setting.fans.right_speed = ini_getl("fans", "right_speed", g_setting_defaults.fans.right_speed, SETTING_INI);

    // source
    g_setting.source.analog_format = ini_getl("source", "analog_format", g_setting_defaults.source.analog_format, SETTING_INI);
    g_setting.source.analog_ratio = ini_getl("source", "analog_ratio", g_setting_defaults.source.analog_ratio, SETTING_INI);
    g_setting.source.hdzero_band = ini_getl("source", "hdzero_band", g_setting_defaults.source.hdzero_band, SETTING_INI);
    g_setting.source.hdzero_bw = ini_getl("source", "hdzero_bw", g_setting_defaults.source.hdzero_bw, SETTING_INI);
    g_setting.source.analog_channel = ini_getl("source", "analog_channel", g_setting_defaults.source.analog_channel, SETTING_INI);
    if (g_setting.scan.channel > HDZERO_CHANNEL_NUM) {
        g_setting.scan.channel = 1;
    }
    // This clamped scan.channel, which belongs to HDZero and had just been
    // checked against its own limit two lines up; the analog channel it was
    // meant to fix went through unchecked and straight into the 48 entry name
    // table. Zero is out of range too: channel2str() indexes channel - 1.
    if ((g_setting.source.analog_channel < 1) ||
        (g_setting.source.analog_channel > ANALOG_CHANNEL_NUM)) {
        g_setting.source.analog_channel = g_setting_defaults.source.analog_channel;
    }

    // favorites
    // The HDZero list keeps the original [favorites] section so lists
    // registered before analog support survive the upgrade.
    settings_load_favorites(&g_setting.favorites.hdzero, &g_setting_defaults.favorites.hdzero,
                            FAVORITES_INI_HDZERO, BASE_CH_NUM);
    settings_load_favorites(&g_setting.favorites.analog, &g_setting_defaults.favorites.analog,
                            FAVORITES_INI_ANALOG, ANALOG_CHANNEL_NUM);

    // autoscan
    g_setting.autoscan.status = ini_getl("autoscan", "status", g_setting_defaults.autoscan.status, SETTING_INI);
    g_setting.autoscan.source = ini_getl("autoscan", "source", g_setting_defaults.autoscan.source, SETTING_INI);
    g_setting.autoscan.last_source = ini_getl("autoscan", "last_source", g_setting_defaults.autoscan.last_source, SETTING_INI);

    // osd
    g_setting.osd.orbit = ini_getl("osd", "orbit", g_setting_defaults.osd.orbit, SETTING_INI);
    g_setting.osd.embedded_mode = ini_getl("osd", "embedded_mode", g_setting_defaults.osd.embedded_mode, SETTING_INI);
    g_setting.osd.startup_visibility = ini_getl("osd", "startup_visibility", g_setting_defaults.osd.startup_visibility, SETTING_INI);

    switch (g_setting.osd.startup_visibility) {
    default:
    case SETTING_OSD_SHOW_AT_STARTUP_SHOW:
        g_setting.osd.is_visible = true;
        settings_put_bool("osd", "is_visible", g_setting.osd.is_visible);
        break;
    case SETTING_OSD_SHOW_AT_STARTUP_HIDE:
        g_setting.osd.is_visible = false;
        settings_put_bool("osd", "is_visible", g_setting.osd.is_visible);
        break;
    case SETTING_OSD_SHOW_AT_STARTUP_LAST:
        g_setting.osd.is_visible = settings_get_bool("osd", "is_visible", g_setting_defaults.osd.is_visible);
        break;
    }

    settings_load_osd_element(&g_setting.osd.element[OSD_GOGGLE_TOPFAN_SPEED], "topfan_speed", &g_setting_defaults.osd.element[OSD_GOGGLE_TOPFAN_SPEED]);
    settings_load_osd_element(&g_setting.osd.element[OSD_GOGGLE_LATENCY_LOCK], "latency_lock", &g_setting_defaults.osd.element[OSD_GOGGLE_LATENCY_LOCK]);
    settings_load_osd_element(&g_setting.osd.element[OSD_GOGGLE_VTX_TEMP], "vtx_temp", &g_setting_defaults.osd.element[OSD_GOGGLE_VTX_TEMP]);
    settings_load_osd_element(&g_setting.osd.element[OSD_GOGGLE_VRX_TEMP], "vrx_temp", &g_setting_defaults.osd.element[OSD_GOGGLE_VRX_TEMP]);
    settings_load_osd_element(&g_setting.osd.element[OSD_GOGGLE_BATTERY_LOW], "battery_low", &g_setting_defaults.osd.element[OSD_GOGGLE_BATTERY_LOW]);
    settings_load_osd_element(&g_setting.osd.element[OSD_GOGGLE_BATTERY_VOLTAGE], "battery_voltage", &g_setting_defaults.osd.element[OSD_GOGGLE_BATTERY_VOLTAGE]);
    settings_load_osd_element(&g_setting.osd.element[OSD_GOGGLE_CLOCK_DATE], "clock_date", &g_setting_defaults.osd.element[OSD_GOGGLE_CLOCK_DATE]);
    settings_load_osd_element(&g_setting.osd.element[OSD_GOGGLE_CLOCK_TIME], "clock_time", &g_setting_defaults.osd.element[OSD_GOGGLE_CLOCK_TIME]);
    settings_load_osd_element(&g_setting.osd.element[OSD_GOGGLE_CHANNEL], "channel", &g_setting_defaults.osd.element[OSD_GOGGLE_CHANNEL]);
    settings_load_osd_element(&g_setting.osd.element[OSD_GOGGLE_SD_REC], "sd_rec", &g_setting_defaults.osd.element[OSD_GOGGLE_SD_REC]);
    settings_load_osd_element(&g_setting.osd.element[OSD_GOGGLE_VLQ], "vlq", &g_setting_defaults.osd.element[OSD_GOGGLE_VLQ]);
    settings_load_osd_element(&g_setting.osd.element[OSD_GOGGLE_ANT0], "ant0", &g_setting_defaults.osd.element[OSD_GOGGLE_ANT0]);
    settings_load_osd_element(&g_setting.osd.element[OSD_GOGGLE_ANT1], "ant1", &g_setting_defaults.osd.element[OSD_GOGGLE_ANT1]);
    settings_load_osd_element(&g_setting.osd.element[OSD_GOGGLE_ANT2], "ant2", &g_setting_defaults.osd.element[OSD_GOGGLE_ANT2]);
    settings_load_osd_element(&g_setting.osd.element[OSD_GOGGLE_ANT3], "ant3", &g_setting_defaults.osd.element[OSD_GOGGLE_ANT3]);
    settings_load_osd_element(&g_setting.osd.element[OSD_GOGGLE_TEMP_TOP], "goggle_temp_top", &g_setting_defaults.osd.element[OSD_GOGGLE_TEMP_TOP]);
    settings_load_osd_element(&g_setting.osd.element[OSD_GOGGLE_TEMP_LEFT], "goggle_temp_left", &g_setting_defaults.osd.element[OSD_GOGGLE_TEMP_LEFT]);
    settings_load_osd_element(&g_setting.osd.element[OSD_GOGGLE_TEMP_RIGHT], "goggle_temp_right", &g_setting_defaults.osd.element[OSD_GOGGLE_TEMP_RIGHT]);

    // power
    g_setting.power.voltage = ini_getl("power", "voltage_mv", g_setting_defaults.power.voltage, SETTING_INI);
    g_setting.power.warning_type = ini_getl("power", "warning_type", g_setting_defaults.power.warning_type, SETTING_INI);
    g_setting.power.cell_count_mode = ini_getl("power", "cell_count_mode", g_setting_defaults.power.cell_count_mode, SETTING_INI);
    g_setting.power.cell_count = ini_getl("power", "cell_count", g_setting_defaults.power.cell_count, SETTING_INI);
    g_setting.power.osd_display_mode = ini_getl("power", "osd_display_mode", g_setting_defaults.power.osd_display_mode, SETTING_INI);
    g_setting.power.power_ana = ini_getl("power", "power_ana_rx", g_setting_defaults.power.power_ana, SETTING_INI);
    g_setting.power.calibration_offset = ini_getl("power", "calibration_offset_mv", g_setting_defaults.power.calibration_offset, SETTING_INI);

    // record
    g_setting.record.mode_manual = settings_get_bool("record", "mode_manual", g_setting_defaults.record.mode_manual);
    g_setting.record.format_ts = settings_get_bool("record", "format_ts", g_setting_defaults.record.format_ts);
    g_setting.record.bitrate_scale = ini_getl("record", "bitrate_scale", g_setting_defaults.record.bitrate_scale, SETTING_INI);
    g_setting.record.osd = settings_get_bool("record", "osd", g_setting_defaults.record.osd);
    g_setting.record.audio = settings_get_bool("record", "audio", g_setting_defaults.record.audio);
    g_setting.record.audio_source = ini_getl("record", "audio_source", g_setting_defaults.record.audio_source, SETTING_INI);
    g_setting.record.naming = ini_getl("record", "naming", g_setting_defaults.record.naming, SETTING_INI);

    // image
    g_setting.image.oled = ini_getl("image", "oled", g_setting_defaults.image.oled, SETTING_INI);
    g_setting.image.brightness = ini_getl("image", "brightness", g_setting_defaults.image.brightness, SETTING_INI);
    g_setting.image.saturation = ini_getl("image", "saturation", g_setting_defaults.image.saturation, SETTING_INI);
    g_setting.image.contrast = ini_getl("image", "contrast", g_setting_defaults.image.contrast, SETTING_INI);
    g_setting.image.auto_off = ini_getl("image", "auto_off", g_setting_defaults.image.auto_off, SETTING_INI);

    // head tracker
    g_setting.ht.enable = settings_get_bool("ht", "enable", g_setting_defaults.ht.enable);
    g_setting.ht.max_angle = ini_getl("ht", "max_angle", g_setting_defaults.ht.max_angle, SETTING_INI);
    g_setting.ht.acc_x = ini_getl("ht", "acc_x", g_setting_defaults.ht.acc_x, SETTING_INI);
    g_setting.ht.acc_y = ini_getl("ht", "acc_y", g_setting_defaults.ht.acc_y, SETTING_INI);
    g_setting.ht.acc_z = ini_getl("ht", "acc_z", g_setting_defaults.ht.acc_z, SETTING_INI);
    g_setting.ht.gyr_x = ini_getl("ht", "gyr_x", g_setting_defaults.ht.gyr_x, SETTING_INI);
    g_setting.ht.gyr_y = ini_getl("ht", "gyr_y", g_setting_defaults.ht.gyr_y, SETTING_INI);
    g_setting.ht.gyr_z = ini_getl("ht", "gyr_z", g_setting_defaults.ht.gyr_z, SETTING_INI);
    g_setting.ht.alarm_state = ini_getl("ht", "alarm_state", g_setting_defaults.ht.alarm_state, SETTING_INI);
    g_setting.ht.alarm_angle = ini_getl("ht", "alarm_angle", g_setting_defaults.ht.alarm_angle, SETTING_INI);

    // elrs
    g_setting.elrs.enable = settings_get_bool("elrs", "enable", g_setting_defaults.elrs.enable);

    // clock
    g_setting.clock.year = ini_getl("clock", "year", g_setting_defaults.clock.year, SETTING_INI);
    g_setting.clock.month = ini_getl("clock", "month", g_setting_defaults.clock.month, SETTING_INI);
    g_setting.clock.day = ini_getl("clock", "day", g_setting_defaults.clock.day, SETTING_INI);
    g_setting.clock.hour = ini_getl("clock", "hour", g_setting_defaults.clock.hour, SETTING_INI);
    g_setting.clock.min = ini_getl("clock", "min", g_setting_defaults.clock.min, SETTING_INI);
    g_setting.clock.sec = ini_getl("clock", "sec", g_setting_defaults.clock.sec, SETTING_INI);
    g_setting.clock.format = ini_getl("clock", "format", g_setting_defaults.clock.format, SETTING_INI);

    // inputs
    g_setting.inputs.roller = ini_getl("inputs", "roller", g_setting_defaults.inputs.roller, SETTING_INI);
    g_setting.inputs.left_click = ini_getl("inputs", "left_click", g_setting_defaults.inputs.left_click, SETTING_INI);
    g_setting.inputs.left_press = ini_getl("inputs", "left_press", g_setting_defaults.inputs.left_press, SETTING_INI);
    g_setting.inputs.right_click = ini_getl("inputs", "right_click", g_setting_defaults.inputs.right_click, SETTING_INI);
    g_setting.inputs.right_press = ini_getl("inputs", "right_press", g_setting_defaults.inputs.right_press, SETTING_INI);
    g_setting.inputs.right_double_click = ini_getl("inputs", "right_double_click", g_setting_defaults.inputs.right_double_click, SETTING_INI);

    // wifi
    g_setting.wifi.enable = settings_get_bool("wifi", "enable", g_setting_defaults.wifi.enable);
    g_setting.wifi.mode = ini_getl("wifi", "mode", g_setting_defaults.wifi.mode, SETTING_INI);
    ini_gets("wifi", "clientid", g_setting_defaults.wifi.clientid, g_setting.wifi.clientid, WIFI_CLIENTID_MAX, SETTING_INI);
    ini_gets("wifi", "ap_ssid", g_setting_defaults.wifi.ssid[0], g_setting.wifi.ssid[0], WIFI_SSID_MAX, SETTING_INI);
    ini_gets("wifi", "ap_passwd", g_setting_defaults.wifi.passwd[0], g_setting.wifi.passwd[0], WIFI_PASSWD_MAX, SETTING_INI);
    ini_gets("wifi", "sta_ssid", g_setting_defaults.wifi.ssid[1], g_setting.wifi.ssid[1], WIFI_SSID_MAX, SETTING_INI);
    ini_gets("wifi", "sta_passwd", g_setting_defaults.wifi.passwd[1], g_setting.wifi.passwd[1], WIFI_PASSWD_MAX, SETTING_INI);
    g_setting.wifi.dhcp = settings_get_bool("wifi", "dhcp", g_setting_defaults.wifi.dhcp);
    ini_gets("wifi", "ip_addr", g_setting_defaults.wifi.ip_addr, g_setting.wifi.ip_addr, WIFI_NETWORK_MAX, SETTING_INI);
    ini_gets("wifi", "netmask", g_setting_defaults.wifi.netmask, g_setting.wifi.netmask, WIFI_NETWORK_MAX, SETTING_INI);
    ini_gets("wifi", "gateway", g_setting_defaults.wifi.gateway, g_setting.wifi.gateway, WIFI_NETWORK_MAX, SETTING_INI);
    ini_gets("wifi", "dns", g_setting_defaults.wifi.dns, g_setting.wifi.dns, WIFI_NETWORK_MAX, SETTING_INI);
    g_setting.wifi.rf_channel = ini_getl("wifi", "rf_channel", g_setting_defaults.wifi.rf_channel, SETTING_INI);
    ini_gets("wifi", "root_pw", g_setting_defaults.wifi.root_pw, g_setting.wifi.root_pw, WIFI_PASSWD_MAX, SETTING_INI);
    g_setting.wifi.ssh = settings_get_bool("wifi", "ssh", g_setting_defaults.wifi.ssh);

    //  no dial under video mode
    g_setting.ease.no_dial = fs_file_exists(NO_DIAL_FILE);

    // speed
    g_setting.speed.fast_menu = settings_get_bool("speed", "fast_menu", g_setting_defaults.speed.fast_menu);
    g_setting.speed.keep_display = settings_get_bool("speed", "keep_display", g_setting_defaults.speed.keep_display);
    g_setting.speed.skip_audio = settings_get_bool("speed", "skip_audio", g_setting_defaults.speed.skip_audio);
    g_setting.speed.boot_display = settings_get_bool("speed", "boot_display", g_setting_defaults.speed.boot_display);
    g_setting.speed.boot_fonts = settings_get_bool("speed", "boot_fonts", g_setting_defaults.speed.boot_fonts);
    g_setting.speed.skip_boot_menu = settings_get_bool("speed", "skip_boot_menu", g_setting_defaults.speed.skip_boot_menu);
    g_setting.speed.async_imu = settings_get_bool("speed", "async_imu", g_setting_defaults.speed.async_imu);
    g_setting.speed.async_display = settings_get_bool("speed", "async_display", g_setting_defaults.speed.async_display);
    g_setting.speed.ui_throttle = settings_get_bool("speed", "ui_throttle", g_setting_defaults.speed.ui_throttle);
    g_setting.speed.label_diff = settings_get_bool("speed", "label_diff", g_setting_defaults.speed.label_diff);
    g_setting.speed.timed_long_press = settings_get_bool("speed", "timed_long_press", g_setting_defaults.speed.timed_long_press);
    g_setting.speed.split_lock = settings_get_bool("speed", "split_lock", g_setting_defaults.speed.split_lock);
    g_setting.speed.menu_antialias_off = settings_get_bool("speed", "menu_antialias_off", g_setting_defaults.speed.menu_antialias_off);
    g_setting.speed.async_tuner = settings_get_bool("speed", "async_tuner", g_setting_defaults.speed.async_tuner);
    g_setting.speed.spi_burst = settings_get_bool("speed", "spi_burst", g_setting_defaults.speed.spi_burst);
    g_setting.speed.skip_wifi_stop = settings_get_bool("speed", "skip_wifi_stop", g_setting_defaults.speed.skip_wifi_stop);
    g_setting.speed.tuner_bus = ini_getl("speed", "tuner_bus", g_setting_defaults.speed.tuner_bus, SETTING_INI);
    if (g_setting.speed.tuner_bus > 2)
        g_setting.speed.tuner_bus = 0;
    g_setting.speed.tuner_bus_duty40 = settings_get_bool("speed", "tuner_bus_duty40", g_setting_defaults.speed.tuner_bus_duty40);
    g_setting.speed.boot_display_early = settings_get_bool("speed", "boot_display_early", g_setting_defaults.speed.boot_display_early);
    g_setting.speed.defer_menu = settings_get_bool("speed", "defer_menu", g_setting_defaults.speed.defer_menu);
    g_setting.speed.fast_efuse = settings_get_bool("speed", "fast_efuse", g_setting_defaults.speed.fast_efuse);
    g_setting.speed.dvr_stop_wait = settings_get_bool("speed", "dvr_stop_wait", g_setting_defaults.speed.dvr_stop_wait);
    g_setting.speed.dvr_start_wait = settings_get_bool("speed", "dvr_start_wait", g_setting_defaults.speed.dvr_start_wait);
    g_setting.speed.menu_async_display = settings_get_bool("speed", "menu_async_display", g_setting_defaults.speed.menu_async_display);
    g_setting.speed.dvr_defer_stop = settings_get_bool("speed", "dvr_defer_stop", g_setting_defaults.speed.dvr_defer_stop);

    // input feedback
    // bug fixes
    g_setting.bugfix.retry_tuner_init = settings_get_bool("bugfix", "retry_tuner_init", g_setting_defaults.bugfix.retry_tuner_init);
    g_setting.bugfix.wait_for_recording = settings_get_bool("bugfix", "wait_for_recording", g_setting_defaults.bugfix.wait_for_recording);
    g_setting.bugfix.short_i2c_timeout = settings_get_bool("bugfix", "short_i2c_timeout", g_setting_defaults.bugfix.short_i2c_timeout);
    g_setting.bugfix.drop_queued_presses = settings_get_bool("bugfix", "drop_queued_presses", g_setting_defaults.bugfix.drop_queued_presses);
    g_setting.bugfix.dvr_give_up = settings_get_bool("bugfix", "dvr_give_up", g_setting_defaults.bugfix.dvr_give_up);
    LOGI("bugfix: retry_tuner_init=%s wait_for_recording=%s short_i2c_timeout=%s drop_queued_presses=%s dvr_give_up=%s",
         g_setting.bugfix.retry_tuner_init ? "on" : "off",
         g_setting.bugfix.wait_for_recording ? "on" : "off",
         g_setting.bugfix.short_i2c_timeout ? "on" : "off",
         g_setting.bugfix.drop_queued_presses ? "on" : "off",
         g_setting.bugfix.dvr_give_up ? "on" : "off");

    g_setting.input.button_beep = settings_get_bool("input", "button_beep", g_setting_defaults.input.button_beep);
    g_setting.input.dial_beep = settings_get_bool("input", "dial_beep", g_setting_defaults.input.dial_beep);
    g_setting.input.long_press_ms = ini_getl("input", "long_press_ms", g_setting_defaults.input.long_press_ms, SETTING_INI);
    if (long_press_choice_index(g_setting.input.long_press_ms) < 0)
        g_setting.input.long_press_ms = g_setting_defaults.input.long_press_ms;
    LOGI("speed: fast_menu=%s keep_display=%s skip_audio=%s boot_display=%s boot_fonts=%s skip_boot_menu=%s async_imu=%s async_display=%s ui_throttle=%s",
         g_setting.speed.fast_menu ? "on" : "off",
         g_setting.speed.keep_display ? "on" : "off",
         g_setting.speed.skip_audio ? "on" : "off",
         g_setting.speed.boot_display ? "on" : "off",
         g_setting.speed.boot_fonts ? "on" : "off",
         g_setting.speed.skip_boot_menu ? "on" : "off",
         g_setting.speed.async_imu ? "on" : "off",
         g_setting.speed.async_display ? "on" : "off",
         g_setting.speed.ui_throttle ? "on" : "off");
    LOGI("speed: label_diff=%s timed_long_press=%s split_lock=%s menu_antialias_off=%s async_tuner=%s spi_burst=%s skip_wifi_stop=%s",
         g_setting.speed.label_diff ? "on" : "off",
         g_setting.speed.timed_long_press ? "on" : "off",
         g_setting.speed.split_lock ? "on" : "off",
         g_setting.speed.menu_antialias_off ? "on" : "off",
         g_setting.speed.async_tuner ? "on" : "off",
         g_setting.speed.spi_burst ? "on" : "off",
         g_setting.speed.skip_wifi_stop ? "on" : "off");
    LOGI("speed: tuner_bus=%s tuner_bus_duty40=%s",
         (const char *[]){"1.2MHz", "800kHz", "200kHz"}[g_setting.speed.tuner_bus],
         g_setting.speed.tuner_bus_duty40 ? "on" : "off");
    LOGI("speed: boot_display_early=%s defer_menu=%s fast_efuse=%s dvr_stop_wait=%s dvr_start_wait=%s menu_async_display=%s dvr_defer_stop=%s",
         g_setting.speed.boot_display_early ? "on" : "off",
         g_setting.speed.defer_menu ? "on" : "off",
         g_setting.speed.fast_efuse ? "on" : "off",
         g_setting.speed.dvr_stop_wait ? "on" : "off",
         g_setting.speed.dvr_start_wait ? "on" : "off",
         g_setting.speed.menu_async_display ? "on" : "off",
         g_setting.speed.dvr_defer_stop ? "on" : "off");

    // storage
    g_setting.storage.logging = settings_get_bool("storage", "logging", g_setting_defaults.storage.logging);

    // analog rssi
    g_setting.analog_rssi.calib_min = ini_getl("analog_rssi", "calib_min", g_setting_defaults.analog_rssi.calib_min, SETTING_INI);
    g_setting.analog_rssi.calib_max = ini_getl("analog_rssi", "calib_max", g_setting_defaults.analog_rssi.calib_max, SETTING_INI);

    // language
    if (!language_config()) {
        g_setting.language.lang = ini_getl("language", "lang", g_setting_defaults.language.lang, SETTING_INI);
    }

    // Check
    if (fs_file_exists(SELF_TEST_FILE)) {
        unlink(SELF_TEST_FILE);
        if (log_file_open(SELF_TEST_FILE)) {
            g_setting.storage.logging = true;
            g_setting.storage.selftest = true;
        }
    } else if (g_setting.storage.logging) {
        app_log_rotate();
        g_setting.storage.logging = log_file_open(APP_LOG_FILE);
    }

#ifdef HDZBOXPRO
    char buf[64];
    char value_str[2] = {0};
    fs_printf("/sys/class/gpio/export", "%d", GPIO_IS_PRO);
    sprintf(buf, "/sys/class/gpio/gpio%d/direction", GPIO_IS_PRO);
    fs_printf(buf, "in");
    usleep(1000 * 100);
    sprintf(buf, "/sys/class/gpio/gpio%d/value", GPIO_IS_PRO);
    FILE *fp = fopen(buf, "r");
    if (!fp) {
        return;
    }
    if (fgets(value_str, sizeof(value_str), fp) == NULL) {
        LOGE("Failed to read GPIO_IS_PRO");
        fclose(fp);
        return;
    }
    fclose(fp);
    if (atoi(value_str)) {
        LOGI("IS NOT PRO");
        g_setting.has_all_features = false;
    } else {
        LOGI("IS PRO");
    }
#endif
}
