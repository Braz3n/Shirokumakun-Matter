/*
 * Matter AC Controller — ZCL attribute change callbacks.
 *
 * When Thermostat (EP1) or FanControl (EP1) attributes change, reads
 * the current state and transmits a full IR command to the AC unit.
 */

#include "ir_driver.h"
#include "ir_protocol.h"

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/ConcreteAttributePath.h>
#include <lib/support/logging/CHIPLogging.h>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(zcl_cb, CONFIG_LOG_DEFAULT_LEVEL);

using namespace chip;
using namespace chip::app;
using namespace chip::app::Clusters;

/* Last-sent AC state — avoids retransmitting the same IR frame. */
static struct AcState last_sent;
static bool           last_sent_valid;
static bool           ir_ready;

/* Map Matter SystemMode to Hitachi AC mode. SystemMode 0 (Off) is no longer
 * used for power — power is driven by the On/Off cluster. */
static AcMode system_mode_to_ac_mode(uint8_t system_mode) {
    switch (system_mode) {
    case 3:
        return AC_MODE_COOLING; /* Cool */
    case 4:
        return AC_MODE_HEATING; /* Heat */
    case 7:
        return AC_MODE_VENTILATION; /* FanOnly */
    case 8:
        return AC_MODE_DEHUMIDIFY; /* Dry */
    default:
        return AC_MODE_COOLING;
    }
}

/* Map Matter FanMode to Hitachi fan speed. */
static FanSpeed fan_mode_to_speed(uint8_t fan_mode) {
    switch (fan_mode) {
    case 0:
        return FAN_SPEED_1; /* Off → lowest */
    case 1:
        return FAN_SPEED_1; /* Low */
    case 2:
        return FAN_SPEED_3; /* Medium */
    case 3:
        return FAN_SPEED_5; /* High */
    case 5:
        return FAN_SPEED_AUTO; /* Auto */
    default:
        return FAN_SPEED_AUTO;
    }
}

void zcl_callbacks_ready(void) {
    using namespace Thermostat::Attributes;
    using namespace FanControl::Attributes;

    /* Snapshot current attribute values so deferred callbacks from
     * Matter stack init don't trigger an IR transmission on boot. */
    bool on_off = false;
    OnOff::Attributes::OnOff::Get(1, &on_off);

    Thermostat::SystemModeEnum system_mode_enum = Thermostat::SystemModeEnum::kCool;
    SystemMode::Get(1, &system_mode_enum);
    uint8_t system_mode = static_cast<uint8_t>(system_mode_enum);

    int16_t setpoint = 2400;
    if (system_mode == 3) {
        OccupiedCoolingSetpoint::Get(1, &setpoint);
    } else if (system_mode == 4) {
        OccupiedHeatingSetpoint::Get(1, &setpoint);
    }

    FanControl::FanModeEnum fan_mode_enum = FanControl::FanModeEnum::kAuto;
    FanMode::Get(1, &fan_mode_enum);

    last_sent.power  = on_off;
    last_sent.mode   = system_mode_to_ac_mode(system_mode);
    last_sent.temp_c = (uint8_t)(setpoint / 100);
    last_sent.fan    = fan_mode_to_speed(static_cast<uint8_t>(fan_mode_enum));

    if (last_sent.temp_c < 16)
        last_sent.temp_c = 16;
    if (last_sent.temp_c > 30)
        last_sent.temp_c = 30;

    last_sent_valid = true;
    ir_ready        = true;
}

static void send_ir_if_changed(void) {
    if (!ir_ready) {
        return;
    }

    using namespace Thermostat::Attributes;
    using namespace FanControl::Attributes;

    /* Read power state from On/Off cluster */
    bool on_off = false;
    OnOff::Attributes::OnOff::Get(1, &on_off);

    /* Read current SystemMode (Cool/Heat/FanOnly — not Off) */
    Thermostat::SystemModeEnum system_mode_enum = Thermostat::SystemModeEnum::kCool;
    SystemMode::Get(1, &system_mode_enum);
    uint8_t system_mode = static_cast<uint8_t>(system_mode_enum);

    /* Read setpoint based on mode */
    int16_t setpoint = 2400; /* default 24.00 C */
    if (system_mode == 3) {  /* Cool */
        OccupiedCoolingSetpoint::Get(1, &setpoint);
    } else if (system_mode == 4) { /* Heat */
        OccupiedHeatingSetpoint::Get(1, &setpoint);
    }

    /* Read fan mode */
    FanControl::FanModeEnum fan_mode_enum = FanControl::FanModeEnum::kAuto;
    FanMode::Get(1, &fan_mode_enum);
    uint8_t fan_mode = static_cast<uint8_t>(fan_mode_enum);

    /* Build AC state */
    struct AcState state;
    state.power  = on_off;
    state.mode   = system_mode_to_ac_mode(system_mode);
    state.temp_c = (uint8_t)(setpoint / 100);
    state.fan    = fan_mode_to_speed(fan_mode);

    /* Clamp temperature to valid range */
    if (state.temp_c < 16)
        state.temp_c = 16;
    if (state.temp_c > 30)
        state.temp_c = 30;

    /* Skip if unchanged */
    if (last_sent_valid && state.power == last_sent.power && state.mode == last_sent.mode &&
        state.temp_c == last_sent.temp_c && state.fan == last_sent.fan) {
        return;
    }

    LOG_INF("IR: power=%d mode=%d temp=%dC fan=%d", state.power, state.mode, state.temp_c,
            state.fan);

    /* Encode and transmit */
    static struct IrPulse pulses[IR_MAX_PULSES];
    uint16_t              count = ac_encode_pulses(&state, pulses);
    ir_dispatch_command(pulses, count);

    last_sent       = state;
    last_sent_valid = true;
}

void MatterPostAttributeChangeCallback(const chip::app::ConcreteAttributePath &attributePath,
                                       uint8_t type, uint16_t size, uint8_t *value) {
    ClusterId clusterId = attributePath.mClusterId;

    if (attributePath.mEndpointId != 1) {
        return;
    }

    if (clusterId == OnOff::Id) {
        ChipLogProgress(Zcl, "OnOff attr 0x%04x changed", attributePath.mAttributeId);
        send_ir_if_changed();
    } else if (clusterId == Thermostat::Id) {
        ChipLogProgress(Zcl, "Thermostat attr 0x%04x changed", attributePath.mAttributeId);
        send_ir_if_changed();
    } else if (clusterId == FanControl::Id) {
        ChipLogProgress(Zcl, "FanControl attr 0x%04x changed", attributePath.mAttributeId);
        send_ir_if_changed();
    }
}
