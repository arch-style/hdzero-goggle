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

// Read and write a SoC register through /sys/class/sunxi_dump, the class the
// goggles' awr and aww scripts use, without starting a shell. False if the
// class is missing or the read does not parse; *value is untouched then.
bool reg_read(uint32_t addr, uint32_t *value);
bool reg_write(uint32_t addr, uint32_t value);

int system_exec(const char *command);
int system_script(const char *script);

#ifdef __cplusplus
}
#endif
