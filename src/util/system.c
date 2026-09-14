#include "system.h"

#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log/log.h"
#include "util/filesystem.h"
#include "util/time.h"

#include <sys/syscall.h>
#include <unistd.h>

#include <log/log.h>

void log_thread_id(const char *name) {
    LOGI("thread: %s is %d", name, (int)syscall(SYS_gettid));
}

bool reg_read(uint32_t addr, uint32_t *value) {
    char cmd[48];
    uint32_t got_addr = 0, got_val = 0;

    snprintf(cmd, sizeof(cmd), "awr 0x%08x 2>/dev/null", addr);

    FILE *fp = popen(cmd, "r");
    if (!fp)
        return false;

    // awr prints "read 0x05070080:0x00000123"; gpadc.c parses the same line.
    int n = fscanf(fp, "read 0x%x:0x%x", &got_addr, &got_val);
    pclose(fp);

    if (n != 2 || got_addr != addr)
        return false;

    *value = got_val;
    return true;
}

// Every one of these forks a shell. On this SoC that is not free, and the
// menu/video switch path runs several, so log how long each one took.
//
// time_ms() rather than gettimeofday(): rtc_init() moves the wall clock from
// 1970 to 2026 partway through start-up, and a wall-clock measurement that
// straddles it reads as tens of millions of milliseconds. The scripts either
// side of rtc_init() are exactly the ones worth timing.

int system_exec(const char *command) {
    LOGI("System Execute: %s", command);

    uint32_t start = time_ms();
    int retval = system(command);

    LOGI("System Execute: %ums for %s", time_ms() - start, command);

    return retval;
}

int system_script(const char *command) {
    LOGI("System Script: %s", command);

    // basename may edit argument
    const char *script = fs_basename(command);

    // Modify command to log std out and error to a temporary file
    char buffer[256];
    snprintf(buffer,
             sizeof(buffer), "%s > /tmp/%s.log 2>&1",
             command,
             script);
    uint32_t start = time_ms();
    int retval = system(buffer);

    LOGI("System Script: %ums for %s", time_ms() - start, command);

    // Read contents of temporary file and use logging framework
    snprintf(buffer, sizeof(buffer), "/tmp/%s.log", script);
    FILE *fp = fopen(buffer, "r");
    if (fp) {
        char *line = NULL;
        size_t len = 0;
        ssize_t read = 0;
        while ((read = getline(&line, &len, fp)) != -1) {
            line[strcspn(line, "\r\n")] = 0;
            LOGD("%s", line);
        }
        if (line) {
            free(line);
        }
        fclose(fp);
    }

    return retval;
}
