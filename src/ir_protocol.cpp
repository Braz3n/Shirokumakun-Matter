/*
 * Hitachi "Shirokuma-kun" AC IR protocol encoder.
 *
 * 53-byte stateless protocol:
 *   - 3-byte preamble
 *   - 25 data bytes each followed by their bitwise complement
 *   - LSB-first bit ordering
 *   - 30ms wakeup burst, then 3.38ms header, then 424 data bits + trail
 *
 * Reference: https://github.com/Braz3n/ShirokumaKunController-rs
 */

#include "ir_protocol.h"

#include <string.h>

/* Timing constants (microseconds). */
#define WAKEUP_MARK   30000U
#define WAKEUP_SPACE  49500U
#define HEADER_MARK    3380U
#define HEADER_SPACE   1700U
#define BIT_MARK        530U
#define ONE_SPACE      1100U
#define ZERO_SPACE      300U
#define TRAIL_MARK     BIT_MARK

/* Build the 25-byte raw data buffer for the Hitachi protocol. */
static void encode_raw(const struct AcState *state, uint8_t raw[25])
{
	memset(raw, 0, 25);

	/* Bytes 0-3: fixed preamble */
	raw[0] = 0x40;
	raw[1] = 0xFF;
	raw[2] = 0xCC;
	raw[3] = 0x92;

	/* Byte 4: update type (always full-state send) */
	raw[4] = 0x13;

	/* Byte 5: temperature */
	raw[5] = state->temp_c << 2;

	/* Bytes 6-10: unused / timer fields */

	/* Byte 11: fan speed (upper nibble) | mode (lower nibble) */
	uint8_t mode_nibble = state->power ? (uint8_t)state->mode
					   : (uint8_t)AC_MODE_HEATING;
	raw[11] = ((uint8_t)state->fan << 4) | mode_nibble;

	/* Byte 12: power/mode flags */
	if (!state->power) {
		raw[12] = 0xE1;
	} else if (state->mode == AC_MODE_HEATING || state->mode == AC_MODE_COOLING) {
		raw[12] = 0xF1;
	} else {
		raw[12] = 0xF0;
	}

	/* Bytes 15-24: fixed tail */
	raw[15] = 0x80;
	raw[16] = 0x03;
	raw[17] = 0x01;
	raw[18] = 0x88;
	raw[21] = 0xFF;
	raw[22] = 0xFF;
	raw[23] = 0xFF;
	raw[24] = 0xFF;
}

/* Build the 53-byte Hitachi command from raw data:
 * 3-byte preamble, then each of 25 raw bytes followed by its complement. */
static void encode_command(const struct AcState *state, uint8_t cmd[53])
{
	uint8_t raw[25];

	encode_raw(state, raw);

	/* Preamble */
	cmd[0] = 0x01;
	cmd[1] = 0x10;
	cmd[2] = 0x00;

	/* Data bytes with complements */
	for (int i = 0; i < 25; i++) {
		cmd[2 * i + 3] = raw[i];
		cmd[2 * i + 4] = ~raw[i];
	}
}

uint16_t ac_encode_pulses(const struct AcState *state, struct IrPulse *pulses)
{
	uint8_t command[53];
	uint16_t idx = 0;

	encode_command(state, command);

	/* Wakeup burst */
	pulses[idx].mark_us  = WAKEUP_MARK;
	pulses[idx].space_us = WAKEUP_SPACE;
	idx++;

	/* Header pulse */
	pulses[idx].mark_us  = HEADER_MARK;
	pulses[idx].space_us = HEADER_SPACE;
	idx++;

	/* Data bits (LSB-first for each byte) */
	for (int b = 0; b < 53; b++) {
		for (int bit = 0; bit < 8; bit++) {
			bool is_one = (command[b] >> bit) & 1;
			pulses[idx].mark_us  = BIT_MARK;
			pulses[idx].space_us = is_one ? ONE_SPACE : ZERO_SPACE;
			idx++;
		}
	}

	/* Trail pulse */
	pulses[idx].mark_us  = TRAIL_MARK;
	pulses[idx].space_us = 0;
	idx++;

	return idx;
}
