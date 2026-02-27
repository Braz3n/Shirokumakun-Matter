/*
 * Hitachi "Shirokuma-kun" AC IR protocol encoder.
 *
 * Encodes a complete AC state (power, mode, temperature, fan speed) into
 * a sequence of IR mark/space pulses.  The protocol is stateless — every
 * command sends the full desired state.
 */

#pragma once

#include "ir_driver.h"
#include <stdint.h>
#include <stdbool.h>

/* AC operating modes (Hitachi byte 11, lower nibble). */
enum AcMode : uint8_t {
	AC_MODE_OFF         = 0x0,
	AC_MODE_VENTILATION = 0x1,
	AC_MODE_COOLING     = 0x3,
	AC_MODE_DEHUMIDIFY  = 0x5,
	AC_MODE_HEATING     = 0x6,
};

/* Fan speed settings (Hitachi byte 11, upper nibble). */
enum FanSpeed : uint8_t {
	FAN_SPEED_1    = 0x1,
	FAN_SPEED_2    = 0x2,
	FAN_SPEED_3    = 0x3,
	FAN_SPEED_4    = 0x4,
	FAN_SPEED_AUTO = 0x5,
	FAN_SPEED_5    = 0x6,
};

/* Complete AC state to be transmitted via IR. */
struct AcState {
	bool     power;
	AcMode   mode;
	uint8_t  temp_c;   /* 16-30 typical */
	FanSpeed fan;
};

/*
 * Maximum number of pulses in an encoded IR frame.
 * Wakeup(1) + Header(1) + 53*8 data bits(424) + trail(1) = 427.
 */
#define IR_MAX_PULSES 430

/**
 * Encode an AC state into IR pulses ready for transmission.
 *
 * @param state  The desired AC state.
 * @param pulses Output buffer (must hold at least IR_MAX_PULSES entries).
 * @return       Number of valid pulses written to the buffer.
 */
uint16_t ac_encode_pulses(const struct AcState *state, struct IrPulse *pulses);
