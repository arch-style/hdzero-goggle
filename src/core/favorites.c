#include "core/favorites.h"

#include "core/channel_scan.h"
#include "driver/dm6302.h"
#include "driver/hardware.h"
#include "ui/page_common.h"
#include "ui/page_scannow.h"

favorites_source_t favorites_source(void) {
#if defined(HDZGOGGLE)
    // The goggle v1 has no built-in analog receiver, so HDZero is the only
    // receiver the dial ever tunes through favorites. Its setting.ini has no
    // analog_module key, which reads as INTERNAL, and with the module bay
    // selected the analog numbering would have come up.
    return FAVORITES_SOURCE_HDZERO;
#endif
    // With Channel Scan the dial counts in HDZero channels on analog too, and
    // the scan decides which receiver shows the channel.
    if (g_source_info.source == SOURCE_AV_MODULE &&
        (g_setting.source.analog_module == SETTING_SOURCES_ANALOG_MODULE_INTERNAL) &&
        !channel_scan_dial())
        return FAVORITES_SOURCE_ANALOG;

    return FAVORITES_SOURCE_HDZERO;
}

setting_favorites_set_t *favorites_set(int set) {
    if ((set < 1) || (set > FAVORITES_SETS))
        set = 1;

    return &g_setting.favorites.set[set - 1];
}

// Analog channel (1-based, channel2str() order: A B E F R L) for each race
// band HDZero channel: R1-R8, E1, F1, F2, F4. The low band's L1-L8 are the
// analog L1-L8. Same correspondence as hdz_ch2index() in elrs.c.
static const uint8_t raceband_analog[BASE_CH_NUM] = {33, 34, 35, 36, 37, 38, 39, 40, 17, 25, 26, 28};

#define ANALOG_F8 32
#define ANALOG_L1 41

uint8_t favorites_analog_channel(uint8_t band, uint8_t hdzero_ch) {
    if (band == SETTING_SOURCES_HDZERO_BAND_LOWBAND)
        return ((hdzero_ch >= 1) && (hdzero_ch <= 8)) ? ANALOG_L1 + hdzero_ch - 1 : 0;

    return ((hdzero_ch >= 1) && (hdzero_ch <= BASE_CH_NUM)) ? raceband_analog[hdzero_ch - 1] : 0;
}

uint8_t favorites_hdzero_channel(uint8_t band, uint8_t analog_ch) {
    if (band == SETTING_SOURCES_HDZERO_BAND_LOWBAND)
        return ((analog_ch >= ANALOG_L1) && (analog_ch < ANALOG_L1 + 8)) ? analog_ch - ANALOG_L1 + 1 : 0;

    for (int i = 0; i < BASE_CH_NUM; i++) {
        if (raceband_analog[i] == analog_ch)
            return i + 1;
    }

    // F8 is 5880 MHz, the same as R7 (freq_table in elrs.c), so HDZero can
    // receive it as R7.
    if (analog_ch == ANALOG_F8)
        return 7;

    return 0;
}

// What `src` would tune for this slot, in its own numbering, or 0 when the
// slot is empty, past the configured count, or has no channel on `src`.
// HDZero channels outside the selected band are skipped rather than mapped,
// so toggling the band back and forth does not silently rewrite what the
// user registered. Slots beyond the count keep their value but take no part
// in tuning.
static uint8_t slot_tune_channel(int set, favorites_source_t src, int slot) {
    const setting_favorites_set_t *fs = favorites_set(set);
    uint8_t ch;

    if (slot >= fs->count)
        return 0;

    ch = fs->channel[slot];
    if ((ch < 1) || (ch > ANALOG_CHANNEL_NUM))
        return 0;

    if (src == FAVORITES_SOURCE_ANALOG)
        return ch;

    return favorites_hdzero_channel(g_setting.source.hdzero_band, ch);
}

// Collects the usable slots, in slot order, into `out`, keeping only the
// first occurrence of each channel. Without this, the same channel in two
// slots makes favorites_step() hand back the channel it was already on, so
// the dial stalls there and never reaches the rest of the list. It is the
// tuned channel that is compared, so on HDZero F8 and R7 count as one.
static int favorites_collect(int set, favorites_source_t src, uint8_t *out) {
    int count = 0;

    for (int i = 0; i < FAVORITES_MAX; i++) {
        uint8_t ch = slot_tune_channel(set, src, i);
        bool seen = false;

        if (ch == 0)
            continue;

        for (int j = 0; j < count; j++) {
            if (out[j] == ch) {
                seen = true;
                break;
            }
        }

        if (!seen)
            out[count++] = ch;
    }

    return count;
}

int favorites_valid_count(int set, favorites_source_t src) {
    uint8_t list[FAVORITES_MAX];
    return favorites_collect(set, src, list);
}

bool favorites_slot_duplicate(int set, int slot) {
    const setting_favorites_set_t *fs = favorites_set(set);

    if ((slot < 0) || (slot >= fs->count) || (fs->channel[slot] == 0))
        return false;

    for (int i = 0; i < slot; i++) {
        if (fs->channel[i] == fs->channel[slot])
            return true;
    }

    return false;
}

bool favorites_active(void) {
    int set = g_setting.favorites.active;

    return (set >= 1) && (set <= FAVORITES_SETS) && (favorites_valid_count(set, favorites_source()) >= 1);
}

uint8_t favorites_step(uint8_t channel, int dir) {
    uint8_t list[FAVORITES_MAX];
    int count = favorites_collect(g_setting.favorites.active, favorites_source(), list);

    if (count == 0)
        return channel;

    for (int i = 0; i < count; i++) {
        if (list[i] == channel) {
            int next = (dir > 0) ? i + 1 : i - 1;
            if (next >= count)
                next = 0;
            else if (next < 0)
                next = count - 1;
            return list[next];
        }
    }

    // Tuned to something outside the list (band change, VTX-driven change):
    // enter the list from whichever end the dial is heading towards.
    return (dir > 0) ? list[0] : list[count - 1];
}
