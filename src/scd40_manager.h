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

/**
 * Initialise the SCD40 sensor and start periodic measurements.
 * Returns 0 on success, negative errno on failure.
 */
int scd40_manager_init(void);
