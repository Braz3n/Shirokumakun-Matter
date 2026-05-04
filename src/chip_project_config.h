/*
 * CHIP project configuration overrides for Matter AC Controller.
 */

#pragma once

#define CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT 0

/* MRP intervals for Thread MTD.
 *
 * NCS v3.3.0 Matter SDK (v2.9.2): GetLocalMRPConfig() returns Missing() when
 * the local config matches GetDefaultMRPConfig() (idle=500ms, active=300ms).
 * Missing() causes CopyTxtRecord to skip SII/SAI/SAT from the operational SRP
 * advertisement, and Apple Home will not initiate CASE without those TXT records.
 *
 * Setting idle to 4300ms (typical Thread MTD polling interval recommendation)
 * makes the config differ from default, so GetLocalMRPConfig() returns Value()
 * and the TXT records are included in the SRP advertisement.
 */
#define CHIP_CONFIG_MRP_LOCAL_IDLE_RETRY_INTERVAL   (4300_ms32)
#define CHIP_CONFIG_MRP_LOCAL_ACTIVE_RETRY_INTERVAL (300_ms32)
