/*
 * Matter AC Controller — application task.
 *
 * Simplified from the Nordic thermostat sample: no DK buttons/LEDs,
 * no temperature sensor manager, no binding handler.  Initializes
 * the Matter stack, IR driver, SCD40 sensor, and runs the task loop.
 */

#include "app_task.h"
#include "hw_pairing.h"
#include "ir_driver.h"
#include "scd40_manager.h"
#include "pdm_manager.h"

#include "app/matter_init.h"
#include "app/task_executor.h"

#include <app/clusters/identify-server/identify-server.h>
#include <setup_payload/OnboardingCodesUtil.h>

#include <zephyr/logging/log.h>

extern void zcl_callbacks_ready(void);

/* nrf_matter_cluster_init_run_all is called by matter_init.cpp::StartServer() in NCS v3.3.0.
 * We don't register any entries via NRF_MATTER_CLUSTER_INIT so this is a no-op. */
extern "C" bool nrf_matter_cluster_init_run_all(void) { return true; }

LOG_MODULE_DECLARE(app, CONFIG_MATTER_LOG_LEVEL);

using namespace ::chip;
using namespace ::chip::app;
using namespace ::chip::DeviceLayer;

namespace {
constexpr EndpointId kThermostatEndpointId = 1;

/* Identify cluster — required by Matter spec, uses logging only (no LED). */
Identify sIdentify = {kThermostatEndpointId, AppTask::IdentifyStartHandler,
                      AppTask::IdentifyStopHandler, Clusters::Identify::IdentifyTypeEnum::kNone};
} /* namespace */

void AppTask::IdentifyStartHandler(Identify *) {
    LOG_INF("Identify start");
}

void AppTask::IdentifyStopHandler(Identify *) {
    LOG_INF("Identify stop");
}

CHIP_ERROR AppTask::Init() {
    /* Derive discriminator + SPAKE2+ passcode from FICR hardware ID. */
    ReturnErrorOnFailure(HwPairing::Init());

    /* Initialize and start the Matter server. */
    ReturnErrorOnFailure(Nrf::Matter::PrepareServer());
    ReturnErrorOnFailure(Nrf::Matter::StartServer());

    /* Initialize IR driver (PWM-based 38kHz carrier on P0.02). */
    int ret = ir_driver_init();
    if (ret) {
        LOG_ERR("IR driver init failed: %d", ret);
    }

    /* Initialize SCD40 sensor (I2C0, 30s polling). */
    ret = scd40_manager_init();
    if (ret) {
        LOG_ERR("SCD40 init failed: %d (sensor may not be connected)", ret);
    }

    /* Initialize PDM microphone (2kHz beep detection for IR ACK). */
    pdm_manager_init();

    /* Enable IR transmission now that init is complete.
     * This prevents MatterPostAttributeChangeCallback from firing
     * IR during Matter stack attribute restoration at boot. */
    zcl_callbacks_ready();

    return CHIP_NO_ERROR;
}

CHIP_ERROR AppTask::StartApp() {
    ReturnErrorOnFailure(Init());

    while (true) {
        Nrf::DispatchNextTask();
    }

    return CHIP_NO_ERROR;
}
