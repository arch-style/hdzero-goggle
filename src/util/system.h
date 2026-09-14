#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

// Log this thread's kernel id under a name. The i2c reports name the threads
// that were holding and waiting for a bus, and a bare number is not an answer
// to "who". Called once at the top of each thread.
void log_thread_id(const char *name);

// Read a SoC register through the awr tool the goggles ship with (the pair of
// aww, which the app already uses to write). False if the tool is missing or
// its output does not parse; *value is untouched then.
bool reg_read(uint32_t addr, uint32_t *value);

int system_exec(const char *command);
int system_script(const char *script);

#ifdef __cplusplus
}
#endif
