#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "core/settings.h"

// Favorite channels let the dial cycle through a short user-picked list
// instead of walking the whole band. There are FAVORITES_SETS sets, one of
// them in use at a time, and each set serves HDZero and analog alike; see
// setting_favorites_set_t.

typedef enum {
    FAVORITES_SOURCE_HDZERO = 0,
    FAVORITES_SOURCE_ANALOG,
} favorites_source_t;

// Which receiver the dial is tuning: analog only while watching the built-in
// analog receiver, HDZero otherwise. Sources with no tuner of their own
// (HDMI in, AV in) report HDZero, which is what they will come back to.
favorites_source_t favorites_source(void);

// Set 1..FAVORITES_SETS.
setting_favorites_set_t *favorites_set(int set);

// The HDZero channel in `band` on the same frequency as analog channel
// `analog_ch`, or 0 when that band has none.
uint8_t favorites_hdzero_channel(uint8_t band, uint8_t analog_ch);

// The analog channel on the same frequency as HDZero channel `hdzero_ch` of
// `band`, or 0 when out of range. Used to carry over the old HDZero list.
uint8_t favorites_analog_channel(uint8_t band, uint8_t hdzero_ch);

// Number of distinct channels `src` can tune among the in-use slots of a set.
// Slots it cannot tune are not counted, nor are repeats of an earlier slot.
int favorites_valid_count(int set, favorites_source_t src);

// True when this slot repeats a channel already held by an earlier in-use
// slot of the same set, and is therefore ignored when tuning.
bool favorites_slot_duplicate(int set, int slot);

// True when favorite tuning should take over the dial: a set is in use and
// at least one of its slots is usable on the current source. A single usable
// entry is deliberate rather than degenerate -- it locks the dial to that one
// channel.
bool favorites_active(void);

// Neighbour of `channel` in the set in use, wrapping around. `channel` and
// the result are in the current source's own numbering (scan.channel for
// HDZero, analog_channel for analog). dir > 0 walks up the list, dir < 0
// walks down. When `channel` is not itself a favorite the walk starts from
// the end the dial came from.
uint8_t favorites_step(uint8_t channel, int dir);

#ifdef __cplusplus
}
#endif
