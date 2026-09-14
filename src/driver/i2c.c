#include "i2c.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h> // for close

#include <sys/syscall.h>

#include <log/log.h>

#include "../core/common.hh"
#include "core/settings.h"
#include "util/system.h"
#include "util/time.h"

#define IIC_PORTS 4

// One lock per port, since the ports are separate controllers: the motion
// sensor on port 1 need not wait for the tuner init on port 2. Each lock is
// a bus mutex behind a turnstile, so a thread that has just released the bus
// cannot take it straight back while another is waiting: the waiter holds the
// turnstile, and the returning thread queues behind it. Without that the
// tuner init, which issues transfers back to back, starved the OLED and
// display set-up on the same port for hundreds of milliseconds.
typedef struct {
    pthread_mutex_t turnstile;
    pthread_mutex_t bus;
} iic_lock_t;

static iic_lock_t g_iic_locks[IIC_PORTS];

// Boots still turn up now and then where the tuner init on port 2 takes ten
// seconds instead of two, and the log has only ever shown the waiting side:
// "wait for the tuner bus 10080ms" says the main thread was held up and
// nothing says by whom. Time every acquisition and, past a threshold, name
// both the thread that waited and the one that handed the bus over. Two
// clock_gettime() calls per transfer, both through the vDSO, and silence on a
// boot where nothing goes wrong.
#define I2C_WAIT_REPORT_MS 100

// And a single transfer that takes this long is not contention at all: it is
// the kernel's own timeout on a device that never answered, with the bus lock
// held for the whole of it. The two reports together say whether a stall was
// somebody else's turn or nobody's answer, and which chip it was.
#define I2C_XFER_REPORT_MS 200

// What the adapter's timeout is set to with the fix on. The default is five
// seconds; the longest transaction here is a few hundred bits.
#define I2C_TIMEOUT_MS 500

static void i2c_xfer_report(int port, uint8_t slave_address, const char *what,
                            uint32_t started_ms) {
    uint32_t took = time_ms() - started_ms;

    if (took >= I2C_XFER_REPORT_MS)
        LOGE("i2c: port %d, addr 0x%02x, %s took %ums", port, slave_address, what, took);
}

static int g_iic_holder[IIC_PORTS];

static int this_thread(void) {
    return (int)syscall(SYS_gettid);
}

void i2c_bus_lock(int port) {
    uint32_t t0 = time_ms();

    pthread_mutex_lock(&g_iic_locks[port].turnstile);
    pthread_mutex_lock(&g_iic_locks[port].bus);
    pthread_mutex_unlock(&g_iic_locks[port].turnstile);

    uint32_t waited = time_ms() - t0;
    int previous = g_iic_holder[port];

    g_iic_holder[port] = this_thread();

    if (waited >= I2C_WAIT_REPORT_MS)
        LOGE("i2c: port %d, thread %d waited %ums, released by thread %d",
             port, g_iic_holder[port], waited, previous);
}

void i2c_bus_unlock(int port) {
    pthread_mutex_unlock(&g_iic_locks[port].bus);
}

static char *IIC_DEVS[IIC_PORTS] = {
    "/dev/i2c-0",
    "/dev/i2c-1",
    "/dev/i2c-2",
    "/dev/i2c-3",
};

static int g_iic_fds[IIC_PORTS];
static bool g_iic_report_once[IIC_PORTS] = {false};

bool iic_is_port_ready(int port) {
    if (port < 0 || port >= IIC_PORTS) {
        LOGE("Port %d contains an invalid range [0=N/A,1=Right,2=Main,3=Left]", port);
        return false;
    } else if (g_iic_fds[port] < 0) {
        if (!g_iic_report_once[port]) {
            g_iic_report_once[port] = true;
            LOGE("Device %d:%s is not available [0=N/A,1=Right,2=Main,3=Left]", port, IIC_DEVS[port]);
        }
        return false;
    }
    return true;
}

uint32_t g_twi2_ccr_default = 0;
bool g_twi2_ccr_has_duty = false;

// Nothing has touched the bus yet, so read the clock register the kernel set
// up, flip bit 7, read it back, and put the original back: a bit that sticks
// exists. Two register reads and two writes through awr/aww, a few tens of
// ms, once. This is the first fact about the bus that comes from the goggles
// rather than from another SoC's manual.
static void twi2_probe(void) {
    uint32_t def, back;
    char cmd[48];

    if (!reg_read(TWI2_CCR_ADDR, &def)) {
        LOGE("i2c: TWI2 CCR could not be read (no awr?)");
        return;
    }

    g_twi2_ccr_default = def;

    snprintf(cmd, sizeof(cmd), "aww 0x%08x 0x%08x", TWI2_CCR_ADDR, def ^ TWI_CCR_DUTY40);
    system(cmd);
    if (reg_read(TWI2_CCR_ADDR, &back))
        g_twi2_ccr_has_duty = ((back ^ def) & TWI_CCR_DUTY40) != 0;
    snprintf(cmd, sizeof(cmd), "aww 0x%08x 0x%08x", TWI2_CCR_ADDR, def);
    system(cmd);

    unsigned n = def & 7, m = (def >> 3) & 0xF;
    LOGI("i2c: TWI2 CCR as the kernel left it 0x%02x = %ukHz (N=%u M=%u), duty bit %s%s",
         def & 0xFF, 24000u / ((1u << n) * (m + 1) * 10), n, m,
         g_twi2_ccr_has_duty ? "present" : "absent",
         g_twi2_ccr_has_duty ? ((def & TWI_CCR_DUTY40) ? ", set (40%)" : ", clear (50%)") : "");
}

void iic_init() {
    for (int i = 0; i < IIC_PORTS; ++i) {
        pthread_mutex_init(&g_iic_locks[i].turnstile, NULL);
        pthread_mutex_init(&g_iic_locks[i].bus, NULL);
    }

    twi2_probe();

    // Offset starts with 1 as it is not referenced thus far.
    for (int i = 1; i < IIC_PORTS; ++i) {
        g_iic_fds[i] = open(IIC_DEVS[i], O_RDONLY);
        iic_is_port_ready(i);

        // In units of 10ms, and only where the adapter honours it. Settings
        // are loaded before this runs.
        if (g_setting.bugfix.short_i2c_timeout && g_iic_fds[i] >= 0 &&
            ioctl(g_iic_fds[i], I2C_TIMEOUT, I2C_TIMEOUT_MS / 10) < 0)
            LOGE("i2c: port %d would not take a %dms timeout", i, I2C_TIMEOUT_MS);
    }
}

static uint8_t iic_read(int i2c_fd, uint8_t slave_address, uint16_t reg_address) {
    struct i2c_rdwr_ioctl_data work_queue;
    struct i2c_msg msgs[2];

    uint8_t val = 0;
    int ret;

    work_queue.nmsgs = 2;
    work_queue.msgs = msgs;

    val = (unsigned char)reg_address;
    (work_queue.msgs[0]).len = 1;
    (work_queue.msgs[0]).flags = 0;
    (work_queue.msgs[0]).addr = slave_address;
    (work_queue.msgs[0]).buf = &val;

    (work_queue.msgs[1]).len = 1;
    (work_queue.msgs[1]).flags = 1;
    (work_queue.msgs[1]).addr = slave_address;
    (work_queue.msgs[1]).buf = &val;

    ret = ioctl(i2c_fd, I2C_RDWR, (unsigned long)&work_queue);
    if (ret < 0) {
        // LOGI("iic_read[%x.%x] failed.",slave_address, reg_address);
        val = 0;
    }
    return val;
}

static void iic_read_n(int i2c_fd, uint8_t slave_address, uint16_t reg_address, uint8_t *reg_data, uint16_t len) {
    struct i2c_rdwr_ioctl_data work_queue;
    struct i2c_msg msgs[2];
    uint8_t val = 0;

    work_queue.nmsgs = 2;
    work_queue.msgs = msgs;

    val = (unsigned char)reg_address;
    (work_queue.msgs[0]).len = 1;
    (work_queue.msgs[0]).flags = 0;
    (work_queue.msgs[0]).addr = slave_address;
    (work_queue.msgs[0]).buf = &val;

    (work_queue.msgs[1]).len = len;
    (work_queue.msgs[1]).flags = 1;
    (work_queue.msgs[1]).addr = slave_address;
    (work_queue.msgs[1]).buf = reg_data;

    ioctl(i2c_fd, I2C_RDWR, (unsigned long)&work_queue);
    // if(ret < 0)
    //     LOGI("iic_read_n[%x.%x] failed.",slave_address, reg_address);
}

static int iic_write(int i2c_fd, uint8_t slave_address, uint16_t reg_address, uint16_t reg_val) {
    struct i2c_rdwr_ioctl_data work_queue;
    struct i2c_msg msgs;
    uint8_t obuf[2];
    int ret;

    work_queue.nmsgs = 1;
    work_queue.msgs = &msgs;
    msgs.buf = obuf;

    (work_queue.msgs[0]).len = 2;
    (work_queue.msgs[0]).flags = 0;
    (work_queue.msgs[0]).addr = slave_address;
    (work_queue.msgs[0]).buf[0] = reg_address;
    (work_queue.msgs[0]).buf[1] = reg_val;

    ret = ioctl(i2c_fd, I2C_RDWR, (unsigned long)&work_queue);
    if (ret < 0) {
        // LOGI("iic_write[%x.%x]<-%x failed.",slave_address, reg_address, reg_val);
        ret = 0;
    }
    return ret;
}

static int iic_write_n(int i2c_fd, uint8_t slave_address, uint8_t reg_address, uint8_t *reg_val, uint16_t len) {
    struct i2c_rdwr_ioctl_data work_queue;
    struct i2c_msg msgs;
    int ret;

    work_queue.nmsgs = 1;
    work_queue.msgs = &msgs;

    if ((work_queue.msgs[0].buf = (unsigned char *)malloc((len + 1) * sizeof(unsigned char))) == NULL) {
        // LOGI("buf memery alloc error...");
        return -1;
    }

    (work_queue.msgs[0]).len = len + 1;
    (work_queue.msgs[0]).flags = 0;
    (work_queue.msgs[0]).addr = slave_address;
    (work_queue.msgs[0]).buf[0] = reg_address;

    for (uint16_t i = 1; i <= len; i++)
        (work_queue.msgs[0]).buf[i] = reg_val[i - 1];

    ret = ioctl(i2c_fd, I2C_RDWR, (unsigned long)&work_queue);
    if (ret < 0) {
        // LOGI("iic_write_n[%x.%x] failed.",slave_address, reg_address);
        ret = 0;
    }
    free(work_queue.msgs[0].buf);
    return ret;
}

// I2C_RDWR_IOCTL_MAX_MSGS is 42 in the kernel; the tuner's SPI bridge needs 7.
#define IIC_BURST_MAX 16

int i2c_write_burst(int port, uint8_t slave_address, const uint8_t *regs, const uint8_t *vals, uint8_t count) {
    struct i2c_rdwr_ioctl_data work_queue;
    struct i2c_msg msgs[IIC_BURST_MAX];
    uint8_t bufs[IIC_BURST_MAX][2];
    int ret;

    if (count == 0 || count > IIC_BURST_MAX)
        return -1;

    if (!iic_is_port_ready(port))
        return -1;

    for (uint8_t i = 0; i < count; i++) {
        bufs[i][0] = regs[i];
        bufs[i][1] = vals[i];
        msgs[i].addr = slave_address;
        msgs[i].flags = 0;
        msgs[i].len = 2;
        msgs[i].buf = bufs[i];
    }

    work_queue.nmsgs = count;
    work_queue.msgs = msgs;

    i2c_bus_lock(port);
    uint32_t started_ms = time_ms();
    ret = ioctl(g_iic_fds[port], I2C_RDWR, (unsigned long)&work_queue);
    int err = errno;
    i2c_xfer_report(port, slave_address, "burst", started_ms);
    i2c_bus_unlock(port);

    if (ret == count)
        return 0;

    // I2C_RDWR answers with the number of messages it managed to send, so a
    // short count is a failure with a plausible-looking return: the tail of
    // the sequence never reached the device, and for the tuner's SPI bridge
    // the tail is the command register. Report it so the caller redoes the
    // whole sequence rather than assuming it landed.
    if (ret >= 0)
        return -EIO;

    return err ? -err : -EIO;
}

uint8_t i2c_read(int port, uint8_t slave_address, uint8_t addr) {
    uint8_t val = 0;

    if (!iic_is_port_ready(port)) {
        return 0;
    }

    i2c_bus_lock(port);
    uint32_t started_ms = time_ms();
    val = iic_read(g_iic_fds[port], slave_address, addr);
    i2c_xfer_report(port, slave_address, "read", started_ms);
    i2c_bus_unlock(port);

    return val;
}

int8_t i2c_read_n(int port, uint8_t slave_address, uint8_t addr, uint8_t *data, uint16_t len) {
    if (!iic_is_port_ready(port)) {
        return -1;
    }

    i2c_bus_lock(port);
    uint32_t started_ms = time_ms();
    iic_read_n(g_iic_fds[port], slave_address, addr, data, len);
    i2c_xfer_report(port, slave_address, "read n", started_ms);
    i2c_bus_unlock(port);

    return 0;
}

int i2c_write(int port, uint8_t slave_address, uint8_t addr, uint8_t val) {
    int ret = -1;

    if (!iic_is_port_ready(port)) {
        return ret;
    }

    i2c_bus_lock(port);
    uint32_t started_ms = time_ms();
    ret = iic_write(g_iic_fds[port], slave_address, addr, val);
    i2c_xfer_report(port, slave_address, "write", started_ms);
    i2c_bus_unlock(port);

    return ret;
}

int8_t i2c_write_n(int port, uint8_t slave_address, uint8_t addr, uint8_t *val, uint16_t len) {
    int ret = -1;

    if (!iic_is_port_ready(port)) {
        return ret;
    }

    i2c_bus_lock(port);
    uint32_t started_ms = time_ms();
    iic_write_n(g_iic_fds[port], slave_address, addr, val, len);
    i2c_xfer_report(port, slave_address, "write n", started_ms);
    i2c_bus_unlock(port);

    return 0;
}
