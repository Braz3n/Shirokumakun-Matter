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

#include <app/DefaultTimerDelegate.h>
#include <app/clusters/identify-server/IdentifyCluster.h>
#include <data-model-providers/codegen/CodegenDataModelProvider.h>
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

/* Identify cluster — new ServerClusterInterface model (NCS v3.3.0 / CHIP SDK 2.9.x).
 * ZAP marks Identify attributes EXTERNAL_STORAGE, so they must be served by a registered
 * ServerClusterInterface; the legacy Identify shim silently swallows Register() errors.
 * Using RegisteredServerCluster directly mirrors the NCS thermostat sample pattern and
 * surfaces any registration failure. Endpoints 1-3 all carry Identify in the ZAP config. */

chip::app::DefaultTimerDelegate gIdTimer1, gIdTimer2, gIdTimer3;
chip::app::RegisteredServerCluster<chip::app::Clusters::IdentifyCluster>
    gIdCluster1(chip::app::Clusters::IdentifyCluster::Config(1, gIdTimer1)),
    gIdCluster2(chip::app::Clusters::IdentifyCluster::Config(2, gIdTimer2)),
    gIdCluster3(chip::app::Clusters::IdentifyCluster::Config(3, gIdTimer3));

} /* namespace */

CHIP_ERROR AppTask::Init() {
    /* Derive discriminator + SPAKE2+ passcode from FICR hardware ID. */
    ReturnErrorOnFailure(HwPairing::Init());

    /* Initialize and start the Matter server. */
    ReturnErrorOnFailure(Nrf::Matter::PrepareServer());
    ReturnErrorOnFailure(Nrf::Matter::StartServer());

    /* Register Identify clusters for all endpoints that declare them in the ZAP config.
     * Must happen after StartServer() so the registry context is set and Startup() fires. */
    {
        auto & reg = CodegenDataModelProvider::Instance().Registry();
        for (auto * entry : { &gIdCluster1.Registration(),
                               &gIdCluster2.Registration(),
                               &gIdCluster3.Registration() }) {
            CHIP_ERROR err = reg.Register(*entry);
            if (err != CHIP_NO_ERROR) {
                LOG_ERR("Identify Register failed: %" CHIP_ERROR_FORMAT, err.Format());
            }
        }
    }

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
