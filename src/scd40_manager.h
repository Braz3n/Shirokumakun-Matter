/*
 * SCD40 CO2/Temperature/Humidity sensor manager.
 *
 * Uses raw I2C (not Zephyr sensor API) since the SCD4x driver may not
 * be available in older NCS versions.
 *
 * Measurement cycle: starts periodic measurement on init, polls every
 * 30 seconds via k_timer.  Results update Matter attributes on
 * endpoints 1-3.
 */

#pragma once

#include <stdint.h>

/**
 * Initialise the SCD40 sensor and start periodic measurements.
 * Returns 0 on success, negative errno on failure.
 */
int scd40_manager_init(void);

struct Scd40Reading {
    uint16_t co2_ppm;
    int16_t  temp_001c;
    uint16_t rh_001pct;
    bool     valid;
};

struct Scd40Reading scd40_manager_get_last_reading(void);
