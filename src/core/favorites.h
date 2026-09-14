#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "core/settings.h"

// Favorite channels let the dial cycle through a short user-picked list
// instead of walking the whole band. HDZero and analog keep separate lists;
// see setting_favorites_t.

typedef enum {
    FAVORITES_SOURCE_HDZERO = 0,
    FAVORITES_SOURCE_ANALOG,
} favorites_source_t;

// Which list applies right now: analog only while watching the built-in
// analog receiver, HDZero otherwise. Sources with no tuner of their own
// (HDMI in, AV in) report HDZero, which is the list they will come back to.
favorites_source_t favorites_source(void);

// The list favorites_source() names, and the highest channel it may hold.
setting_favorites_list_t *favorites_list(void);
uint8_t favorites_channel_max(void);

// Name of the current source, for the settings page.
const char *favorites_source_name(void);

// The same three for a named source rather than the one being watched. The
// settings page edits either list whatever is on screen, so it asks by source;
// everything that tunes uses the plain forms above, which are these with
// favorites_source().
setting_favorites_list_t *favorites_list_of(favorites_source_t src);
uint8_t favorites_channel_max_of(favorites_source_t src);
const char *favorites_source_name_of(favorites_source_t src);

// Number of distinct channels held by the slots that are in use and valid for
// the current source. Repeats of an earlier slot are not counted.
int favorites_valid_count(void);
int favorites_valid_count_of(favorites_source_t src);

// True when this slot repeats a channel already held by an earlier in-use
// slot, and is therefore ignored when tuning.
bool favorites_slot_duplicate(int slot);
bool favorites_slot_duplicate_of(favorites_source_t src, int slot);

// True when favorite tuning should take over the dial: the feature is enabled
// for the current source and at least one slot is usable. A single usable
// entry is deliberate rather than degenerate -- it locks the dial to that one
// channel.
bool favorites_active(void);

// Neighbour of `channel` in the favorite list, wrapping around.
// dir > 0 walks up the list, dir < 0 walks down. When `channel` is not itself
// a favorite the walk starts from the end the dial came from.
uint8_t favorites_step(uint8_t channel, int dir);

#ifdef __cplusplus
}
#endif
