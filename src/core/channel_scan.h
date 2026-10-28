#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

// Channel Scan: the dial picks a channel, R4 say, and the click finds out
// what is on air there -- HDZero at the bandwidth in use, HDZero at the other
// one, then analog -- and tunes to the first that answers. The HDZero monitor
// changes channel this way. Off by default; [input] channel_scan.
//
// While it is on, the dial counts in HDZero channels (R1-R8 E1 F1 F2 F4, or
// L1-L8) on analog as well, so the channel is one thing whichever receiver
// ends up showing it, and g_setting.scan.channel is where the dial is.

// True when the dial counts in HDZero channels and a click scans: the option
// is on and the source being watched is one the scan can tune.
bool channel_scan_dial(void);

// True from channel_scan_start() until the scan has tuned somewhere.
bool channel_scan_busy(void);

// Scan HDZero channel `hdz_ch` (1-based, in the selected band). Returns at
// once: the scan runs on its own thread, which takes lvgl_mutex for the whole
// of it, as a source switch does, and calls tune_channel_done() at the end.
// False, and nothing started, while a scan is already running. Not on the
// caller's thread because a right button action runs on a UART thread, and
// the scan needs that thread to read the receivers' answers.
bool channel_scan_start(uint8_t hdz_ch);

// With Channel Scan on, the analog receiver follows the dial: tunes
// g_setting.source.analog_channel to the frequency of g_setting.scan.channel.
// For the switches to analog, so Toggle source lands on the same channel.
void channel_scan_carry_to_analog(void);

// The analog channel (1-based, channel2str() order) on the same frequency as
// HDZero channel `hdz_ch` of `band`, or 0 when out of range. The same table
// as hdz_ch2index() in elrs.c.
uint8_t channel_scan_analog_of(uint8_t band, uint8_t hdz_ch);

#ifdef __cplusplus
}
#endif
