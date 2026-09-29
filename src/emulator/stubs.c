#include "player/avplay.h"
#include "player/vigrab.h"
#include "player/vosend.h"

// The player decodes on the goggle's hardware; the emulator plays nothing,
// and the player's screen says so.
AvPlay_t *avplay_open(const char *path) { return NULL; }
void avplay_close(AvPlay_t *ap) {}
int avplay_duration_ms(AvPlay_t *ap) { return 0; }
int avplay_frame_ms(AvPlay_t *ap) { return 16; }
int avplay_position_ms(AvPlay_t *ap) { return 0; }
int avplay_state(AvPlay_t *ap) { return AVPLAY_PAUSED; }
bool avplay_has_audio(AvPlay_t *ap) { return false; }
bool avplay_resuming(AvPlay_t *ap) { return false; }
void avplay_play(AvPlay_t *ap, int divider) {}
void avplay_pause(AvPlay_t *ap) {}
bool avplay_step(AvPlay_t *ap, bool forward) { return false; }
bool avplay_seek(AvPlay_t *ap, int ms) { return false; }
void avplay_set_screen(int width, int height) {}
void avplay_set_scan(int mode) {}
void avplay_set_colorspace(int mode) {}

const char *vosend_mode_name(int mode) { return "?"; }
int vosend_mode_parse(const char *name) { return -1; }
bool vosend_run(const char *path, const vosend_opts_t *opts) { return false; }

bool vigrab_save(const char *name, const char *path, int width, int height, int fps, int skip) { return false; }
void vigrab_close(void) {}

#if HDZGOGGLE
void Display_HDZ(int mode, int is_43) {}
#elif HDZBOXPRO
void Display_720P90(int mode) {}
void Display_720P60_50(int mode, uint8_t is_43) {}
void Display_1080P30(int mode) {}
void Display_1080P24(int mode) {}
#endif
