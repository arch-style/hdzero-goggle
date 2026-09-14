#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#include "bmi2_defs.h"

void init_bmi270();

// True only if the sensor is configured and running. The motion timer must
// not be let loose otherwise: get_bmi270() waits for a data-ready flag in an
// unbounded loop, holding the I2C mutex.
bool enable_bmi270();

void disable_bmi270();

void get_bmi270(struct bmi2_sens_data* sensor_data);

float acc_to_mps2(int16_t acc);

float acc_to_g(int16_t val);

float gyr_to_dps(int16_t gyr);

#ifdef __cplusplus
}
#endif
