#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

void iic_init();
uint8_t i2c_read(int port, uint8_t slave_address, uint8_t addr);
int i2c_write(int port, uint8_t slave_address, uint8_t addr, uint8_t val);

int8_t i2c_read_n(int port, uint8_t slave_address, uint8_t addr, uint8_t *data, uint16_t len);
int8_t i2c_write_n(int port, uint8_t slave_address, uint8_t addr, uint8_t *val, uint16_t len);

// Several single-register writes to one device in a single I2C_RDWR ioctl,
// one message per register, a repeated START between them. Returns -errno if
// the driver refuses the transaction, so callers can fall back to one write
// each.
int i2c_write_burst(int port, uint8_t slave_address, const uint8_t *regs, const uint8_t *vals, uint8_t count);

// The per-port lock every transfer takes. Exposed so a caller can change a
// bus's clock with no transfer in flight on it.
void i2c_bus_lock(int port);
void i2c_bus_unlock(int port);

// Port 2 is TWI2, and this is its clock register (TWI_CCR): bits 2:0 CLK_N,
// 6:3 CLK_M, SCL = 24MHz / (2^N * (M+1) * 10). On the H616-era TWI bit 7 is
// CLK_DUTY (1 = 40% high, the reset default there); on the A10/H3-era TWI
// the bit does not exist. Which of the two this SoC is gets probed at
// iic_init(), because nobody has published the V5's manual.
#define TWI2_CCR_ADDR 0x05002814
#define TWI_CCR_DUTY40 0x80
extern uint32_t g_twi2_ccr_default; // as the kernel left it, before anything else
extern bool g_twi2_ccr_has_duty;    // bit 7 reads back after being written

#define BMI_I2C_WRITE(addr, val, len) i2c_write_n(1, 0x68, addr, val, len)
#define BMI_I2C_READ(addr, val, len)  i2c_read_n(1, 0x68, addr, val, len)

///////////////////////////////////////////////////////////////////////////////
// I2C devices
//
// Name   Ports    Device    Address
// R_I2C	 1      IT66121	    0x4C (HDMI Out)
//	            MCP3021	    0x4D
//	            BMI270	    0x68
//	            NCT75	    0x48
//
// M_I2C	 2      Main FPGA	0x64
//	            AL FPGA	    0x65
//	            GM7150	    0x5D
//	            NCT75	    0x48
//
// L_I2C	 3      IT66021	    0x49 (HDMI In)
//	            NCT75	    0x48
//

#define ADDR_IT66121 0x4C
#define ADDR_MCP3021 0x4D
#define ADDR_BMI270  0x68
#define ADDR_NCT75   0x48

#define ADDR_FPGA 0x64
#define ADDR_AL   0x65

#if defined HDZGOGGLE
#define ADDR_TP2825 0x44
#elif defined HDZBOXPRO
#define ADDR_TP2825 0x45
#elif defined HDZGOGGLE2
#define ADDR_TP2825 0x44
#endif

#define ADDR_IT66021 0x49

#define I2C_Write(s, a, d) i2c_write(2, s, a, d)
#define I2C_Read(s, a)     i2c_read(2, s, a)
#define I2C_Write_Burst(s, regs, vals, n) i2c_write_burst(2, s, regs, vals, n)

#define I2C_R_Write(s, a, d) i2c_write(1, s, a, d)
#define I2C_R_Read(s, a)     i2c_read(1, s, a)

#define I2C_L_Write(s, a, d) i2c_write(3, s, a, d)
#define I2C_L_Read(s, a)     i2c_read(3, s, a)

#ifdef __cplusplus
}
#endif
