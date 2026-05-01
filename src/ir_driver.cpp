/*
 * IR transmit driver for nRF52840 — PWM-based 38kHz carrier.
 *
 * Carrier ON:  PWM at 38kHz, 33% duty (falling polarity, compare=140)
 * Carrier OFF: rising polarity, compare=0 → output always LOW
 *
 * The nRF52840 PWM has a quirk: with falling polarity and compare=0
 * the output is always HIGH (not LOW).  We use rising polarity with
 * compare=0 for true 0%.
 *
 * We write duty values directly via NRF_PWM0 registers to bypass the
 * Zephyr PWM API's inability to express this polarity trick.
 */

#include "ir_driver.h"
#include "pdm_manager.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <nrfx_pwm.h>
#include <hal/nrf_pwm.h>

#include <app-common/zap-generated/attributes/Accessors.h>
#include <platform/CHIPDeviceLayer.h>

LOG_MODULE_REGISTER(ir_driver, CONFIG_LOG_DEFAULT_LEVEL);

using namespace chip::app::Clusters;
using namespace chip::DeviceLayer;

#define IR_RETRY_COUNT    3
#define IR_ACK_TIMEOUT_MS 1000

/* 38kHz carrier constants (PWM clock = 16MHz, prescaler DIV_1) */
static const uint16_t ir_countertop = 421U; /* 16 000 000 / 38 000 ≈ 421 → 38 004 Hz */

/*
 * nRF52840 PWM duty register — bit 15 selects compare polarity:
 *   0 = falling (output HIGH at period start, LOW at compare)
 *   1 = rising  (output LOW at period start, HIGH at compare)
 */
#define CARRIER_ON   140U          /* falling polarity, 140/421 ≈ 33% */
#define CARRIER_OFF  (0x8000U)     /* rising polarity, compare=0 → always LOW */

static nrf_pwm_values_common_t duty_value;
static nrf_pwm_sequence_t seq;

int ir_driver_init(void)
{
	NRF_PWM0->ENABLE = 1;

	/* Prescaler DIV_1 → 16 MHz clock */
	NRF_PWM0->PRESCALER = NRF_PWM_CLK_16MHz;
	NRF_PWM0->COUNTERTOP = ir_countertop;
	NRF_PWM0->MODE = NRF_PWM_MODE_UP;
	NRF_PWM0->DECODER = (NRF_PWM_LOAD_COMMON << PWM_DECODER_LOAD_Pos) |
			     (NRF_PWM_STEP_AUTO << PWM_DECODER_MODE_Pos);
	NRF_PWM0->LOOP = 0;

	/* Point DMA at our single duty value */
	duty_value = CARRIER_OFF;
	seq.values.p_common = &duty_value;
	seq.length = 1;
	seq.repeats = 0;
	seq.end_delay = 0;
	NRF_PWM0->SEQ[0].PTR = (uint32_t)&duty_value;
	NRF_PWM0->SEQ[0].CNT = 1;
	NRF_PWM0->SEQ[0].REFRESH = 0;
	NRF_PWM0->SEQ[0].ENDDELAY = 0;

	/* Connect channel 0 to P0.02 (IR LED). */
	NRF_PWM0->PSEL.OUT[0] = (0 << PWM_PSEL_OUT_PORT_Pos) |  /* Port 0 */
				 (2 << PWM_PSEL_OUT_PIN_Pos)  |   /* Pin 2 */
				 (PWM_PSEL_OUT_CONNECT_Connected << PWM_PSEL_OUT_CONNECT_Pos);
	NRF_PWM0->PSEL.OUT[1] = PWM_PSEL_OUT_CONNECT_Disconnected << PWM_PSEL_OUT_CONNECT_Pos;
	NRF_PWM0->PSEL.OUT[2] = PWM_PSEL_OUT_CONNECT_Disconnected << PWM_PSEL_OUT_CONNECT_Pos;
	NRF_PWM0->PSEL.OUT[3] = PWM_PSEL_OUT_CONNECT_Disconnected << PWM_PSEL_OUT_CONNECT_Pos;

	/* Start with carrier off */
	duty_value = CARRIER_OFF;
	__DMB();
	NRF_PWM0->EVENTS_SEQEND[0] = 0;
	NRF_PWM0->TASKS_SEQSTART[0] = 1;
	while (!NRF_PWM0->EVENTS_SEQEND[0]) {}

	LOG_INF("IR driver initialised (P0.02, 38kHz)");
	return 0;
}

static void load_duty(void)
{
	__DMB();
	NRF_PWM0->EVENTS_SEQEND[0] = 0;
	NRF_PWM0->TASKS_SEQSTART[0] = 1;
	while (!NRF_PWM0->EVENTS_SEQEND[0]) {}
}

void ir_transmit(const struct IrPulse *pulses, uint16_t count)
{
	LOG_INF("IR: transmitting %u pulses", count);

	for (uint16_t i = 0; i < count; i++) {
		/* Mark: 38kHz carrier at 33% duty */
		duty_value = CARRIER_ON;
		load_duty();
		k_busy_wait(pulses[i].mark_us);

		/* Space: carrier off */
		duty_value = CARRIER_OFF;
		load_duty();
		if (pulses[i].space_us > 0) {
			k_busy_wait(pulses[i].space_us);
		}
	}

	/* Ensure carrier is off when done */
	duty_value = CARRIER_OFF;
	load_duty();

	LOG_INF("IR: transmission complete");
}

static void update_ep4_state(bool failed)
{
	PlatformMgr().LockChipStack();
	BooleanState::Attributes::StateValue::Set(4, failed);
	PlatformMgr().UnlockChipStack();
}

bool ir_send_command(const struct IrPulse *pulses, uint16_t count)
{
	for (int attempt = 1; attempt <= IR_RETRY_COUNT; attempt++) {
		pdm_manager_start_listen();
		ir_transmit(pulses, count);
		if (pdm_manager_collect_ack(IR_ACK_TIMEOUT_MS)) {
			LOG_INF("IR: ACK received on attempt %d/%d", attempt, IR_RETRY_COUNT);
			update_ep4_state(false);
			return true;
		}
		LOG_WRN("IR: no ACK on attempt %d/%d", attempt, IR_RETRY_COUNT);
	}
	LOG_ERR("IR: command failed after %d attempts", IR_RETRY_COUNT);
	update_ep4_state(true);
	return false;
}
