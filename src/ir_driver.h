/*
 * IR transmit driver for nRF52840.
 *
 * Uses the PWM peripheral to generate a 38kHz carrier.  Mark/space
 * timing is handled by k_busy_wait() for microsecond precision.
 */

#pragma once

#include <stdint.h>
#include <zephyr/device.h>

/* A single mark/space pulse pair in microseconds. */
struct IrPulse {
	uint16_t mark_us;
	uint16_t space_us;
};

/**
 * Initialise the IR driver.  Call once at boot.
 * Returns 0 on success, negative errno on failure.
 */
int ir_driver_init(void);

/**
 * Transmit a sequence of mark/space pulses.
 * Blocks the calling thread for the duration of transmission (~1.2 s
 * for a full Hitachi frame).
 */
void ir_transmit(const struct IrPulse *pulses, uint16_t count);
