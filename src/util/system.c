#include "system.h"

#include <fcntl.h>
#include <libgen.h>
#include <pthread.h>
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

// The goggles' aww and awr are shell scripts around this sysfs class (the
// kernel's drivers/char/dump_reg): aww is `echo addr val > write` and a
// read-back, awr is `echo addr > dump; cat dump`. Run through system() that
// is two shells and a cat, 8-21ms a write on goggle 2, and boot 161 had ten
// of them between the tuner and the picture. Writing the same strings here
// is the same kernel call without the shells.
//
// "dump" holds one address for the whole system and "write" one group, so
// the app's own threads take turns. A script running awr or aww at the same
// moment could still interleave, as it always could.
#define SUNXI_DUMP "/sys/class/sunxi_dump/"

static pthread_mutex_t reg_lock = PTHREAD_MUTEX_INITIALIZER;

static bool sysfs_puts(const char *path, const char *s) {
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return false;

    size_t len = strlen(s);
    bool ok = write(fd, s, len) == (ssize_t)len;
    close(fd);
    return ok;
}

bool reg_read(uint32_t addr, uint32_t *value) {
    char buf[32];
    uint32_t got = 0;
    bool ok = false;

    snprintf(buf, sizeof(buf), "0x%08x\n", addr);

    pthread_mutex_lock(&reg_lock);
    if (sysfs_puts(SUNXI_DUMP "dump", buf)) {
        int fd = open(SUNXI_DUMP "dump", O_RDONLY);
        if (fd >= 0) {
            // A single register comes back as "0x%08x\n".
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n > 0) {
                buf[n] = '\0';
                ok = sscanf(buf, "0x%x", &got) == 1;
            }
        }
    }
    pthread_mutex_unlock(&reg_lock);

    if (!ok) {
        LOGE("reg read 0x%08x failed", addr);
        return false;
    }

    *value = got;
    return true;
}

bool reg_write(uint32_t addr, uint32_t value) {
    char buf[32];

    snprintf(buf, sizeof(buf), "0x%08x 0x%08x\n", addr, value);

    pthread_mutex_lock(&reg_lock);
    bool ok = sysfs_puts(SUNXI_DUMP "write", buf);
    pthread_mutex_unlock(&reg_lock);

    if (ok)
        LOGI("reg write 0x%08x 0x%08x", addr, value);
    else
        LOGE("reg write 0x%08x 0x%08x failed", addr, value);
    return ok;
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
