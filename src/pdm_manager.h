/*
 * PDM microphone manager — on-demand 2kHz ACK detector.
 *
 * Used to confirm that the AC unit received an IR command (the AC beeps
 * at 2kHz on successful receipt).  PDM capture is only active during the
 * listen window opened by the IR driver.
 *
 * EP4 (Contact Sensor / Boolean State) reflects command delivery status:
 *   StateValue = false → "Closed" (last command acknowledged)
 *   StateValue = true  → "Open"   (last command failed — no ACK)
 *
 * Must be called after Matter StartServer() so the dynamic endpoint can
 * be registered.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

void pdm_manager_init(void);
void pdm_manager_start_listen(void);
bool pdm_manager_collect_ack(uint32_t timeout_ms);

void  pdm_manager_verbose_start(void);
void  pdm_manager_verbose_stop(void);
void  pdm_manager_set_threshold(float t);
float pdm_manager_get_threshold(void);

void pdm_manager_set_ep4_state(bool v);
