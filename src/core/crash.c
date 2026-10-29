// Crash recorder.
//
// The app died twice mid-test with nothing in the log: the log is written out
// by a thread once a second, so the last second before a crash never reaches
// the card, and nothing said which thread or which line. This writes what is
// needed to find the line after the fact. Only async-signal-safe calls:
// open/write/read/close, and formatting by hand.

#include "crash.h"

#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/ucontext.h>
#include <unistd.h>

#include <log/log.h>

#define CRASH_LOG "/mnt/extsd/fork-deletable/crash.log"

static void put(int fd, const char *s) {
    write(fd, s, strlen(s));
}

static void put_hex(int fd, const char *name, unsigned long v) {
    char buf[32];
    int n = 0;
    buf[n++] = ' ';
    for (const char *p = name; *p; p++)
        buf[n++] = *p;
    buf[n++] = '=';
    buf[n++] = '0';
    buf[n++] = 'x';
    for (int i = 7; i >= 0; i--)
        buf[n++] = "0123456789abcdef"[(v >> (i * 4)) & 0xf];
    write(fd, buf, n);
}

static void put_dec(int fd, long v) {
    char buf[24];
    int n = sizeof(buf);
    int neg = v < 0;
    unsigned long u = neg ? -v : v;
    do {
        buf[--n] = '0' + u % 10;
        u /= 10;
    } while (u && n > 1);
    if (neg)
        buf[--n] = '-';
    write(fd, buf + n, sizeof(buf) - n);
}

static void crash_handler(int sig, siginfo_t *si, void *ctx) {
    // First thing: the log's last second.
    log_crash_flush();

    int fd = open(CRASH_LOG, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0) {
        ucontext_t *uc = (ucontext_t *)ctx;
        char name[20] = {0};
        prctl(PR_GET_NAME, name, 0, 0, 0);

        put(fd, "=== crash: signal ");
        put_dec(fd, sig);
        put(fd, " code ");
        put_dec(fd, si->si_code);
        put(fd, " thread ");
        put_dec(fd, (long)syscall(224 /* gettid on ARM EABI */));
        put(fd, " '");
        put(fd, name);
        put(fd, "'\n");
        put_hex(fd, "addr", (unsigned long)si->si_addr);
        put_hex(fd, "pc", uc->uc_mcontext.arm_pc);
        put_hex(fd, "lr", uc->uc_mcontext.arm_lr);
        put_hex(fd, "sp", uc->uc_mcontext.arm_sp);
        put_hex(fd, "r0", uc->uc_mcontext.arm_r0);
        put_hex(fd, "r1", uc->uc_mcontext.arm_r1);
        put_hex(fd, "r2", uc->uc_mcontext.arm_r2);
        put_hex(fd, "r3", uc->uc_mcontext.arm_r3);
        put(fd, "\nstack:");

        // Return addresses are somewhere in here; crash-decode.sh keeps the
        // ones that fall inside code.
        unsigned long *sp = (unsigned long *)uc->uc_mcontext.arm_sp;
        for (int i = 0; i < 256; i++) {
            if (i % 8 == 0)
                put(fd, "\n");
            put_hex(fd, "", sp[i]);
        }
        put(fd, "\nmaps:\n");

        int mf = open("/proc/self/maps", O_RDONLY);
        if (mf >= 0) {
            char buf[512];
            ssize_t n;
            while ((n = read(mf, buf, sizeof(buf))) > 0)
                write(fd, buf, n);
            close(mf);
        }
        put(fd, "=== end\n");
        fsync(fd);
        close(fd);
    }

    // Die the way we would have.
    signal(sig, SIG_DFL);
    raise(sig);
}

void crash_handler_install(void) {
    static char altstack[16384];
    stack_t ss = {.ss_sp = altstack, .ss_size = sizeof(altstack), .ss_flags = 0};
    sigaltstack(&ss, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);

    const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
    for (unsigned i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
        sigaction(sigs[i], &sa, NULL);
}
