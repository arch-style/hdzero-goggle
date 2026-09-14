#include "core/favorites.h"

#include "driver/hardware.h"
#include "ui/page_common.h"
#include "ui/page_scannow.h"

favorites_source_t favorites_source(void) {
    if (g_source_info.source == SOURCE_AV_MODULE &&
        (g_setting.source.analog_module == SETTING_SOURCES_ANALOG_MODULE_INTERNAL))
        return FAVORITES_SOURCE_ANALOG;

    return FAVORITES_SOURCE_HDZERO;
}

setting_favorites_list_t *favorites_list_of(favorites_source_t src) {
    if (src == FAVORITES_SOURCE_ANALOG)
        return &g_setting.favorites.analog;

    return &g_setting.favorites.hdzero;
}

uint8_t favorites_channel_max_of(favorites_source_t src) {
    if (src == FAVORITES_SOURCE_ANALOG)
        return ANALOG_CHANNEL_NUM;

    return HDZERO_CHANNEL_NUM;
}

const char *favorites_source_name_of(favorites_source_t src) {
    return (src == FAVORITES_SOURCE_ANALOG) ? "Analog" : "HDZero";
}

setting_favorites_list_t *favorites_list(void) {
    return favorites_list_of(favorites_source());
}

uint8_t favorites_channel_max(void) {
    return favorites_channel_max_of(favorites_source());
}

const char *favorites_source_name(void) {
    return favorites_source_name_of(favorites_source());
}

// Slots pointing past the end of the current band (F1 while on the low band,
// say) are skipped rather than clamped, so toggling the band back and forth
// does not silently rewrite what the user registered. Slots beyond the
// configured count keep their value but take no part in tuning.
static bool slot_usable_of(favorites_source_t src, int slot) {
    const setting_favorites_list_t *list = favorites_list_of(src);
    uint8_t ch;

    if (slot >= list->count)
        return false;

    ch = list->channel[slot];
    return (ch >= 1) && (ch <= favorites_channel_max_of(src));
}

// Collects the usable slots, in slot order, into `out`, keeping only the
// first occurrence of each channel. Without this, the same channel in two
// slots makes favorites_step() hand back the channel it was already on, so
// the dial stalls there and never reaches the rest of the list.
static int favorites_collect_of(favorites_source_t src, uint8_t *out) {
    const setting_favorites_list_t *list = favorites_list_of(src);
    int count = 0;

    for (int i = 0; i < FAVORITES_MAX; i++) {
        if (!slot_usable_of(src, i))
            continue;

        uint8_t ch = list->channel[i];
        bool seen = false;

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

int favorites_valid_count_of(favorites_source_t src) {
    uint8_t list[FAVORITES_MAX];
    return favorites_collect_of(src, list);
}

int favorites_valid_count(void) {
    return favorites_valid_count_of(favorites_source());
}

bool favorites_slot_duplicate_of(favorites_source_t src, int slot) {
    const setting_favorites_list_t *list = favorites_list_of(src);

    if ((slot < 0) || (slot >= FAVORITES_MAX) || !slot_usable_of(src, slot))
        return false;

    uint8_t ch = list->channel[slot];

    for (int i = 0; i < slot; i++) {
        if (slot_usable_of(src, i) && (list->channel[i] == ch))
            return true;
    }

    return false;
}

bool favorites_slot_duplicate(int slot) {
    return favorites_slot_duplicate_of(favorites_source(), slot);
}

bool favorites_active(void) {
    return favorites_list()->enable && (favorites_valid_count() >= 1);
}

uint8_t favorites_step(uint8_t channel, int dir) {
    uint8_t list[FAVORITES_MAX];
    int count = favorites_collect_of(favorites_source(), list);

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
