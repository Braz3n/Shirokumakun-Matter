/*
 * Matter AC Controller — application task.
 *
 * Simplified from the Nordic thermostat sample: no DK buttons/LEDs,
 * no temperature sensor manager, no binding handler.  Initializes
 * the Matter stack, IR driver, SCD40 sensor, and runs the task loop.
 */

#include "app_task.h"
#include "ir_driver.h"
#include "scd40_manager.h"

#include "app/matter_init.h"
#include "app/task_executor.h"

#include <app/clusters/identify-server/identify-server.h>
#include <app/server/OnboardingCodesUtil.h>

#include <zephyr/logging/log.h>

extern void zcl_callbacks_ready(void);

LOG_MODULE_DECLARE(app, CONFIG_MATTER_LOG_LEVEL);

using namespace ::chip;
using namespace ::chip::app;
using namespace ::chip::DeviceLayer;

namespace
{
constexpr EndpointId kThermostatEndpointId = 1;

/* Identify cluster — required by Matter spec, uses logging only (no LED). */
Identify sIdentify = { kThermostatEndpointId, AppTask::IdentifyStartHandler,
		       AppTask::IdentifyStopHandler,
		       Clusters::Identify::IdentifyTypeEnum::kNone };
} /* namespace */

void AppTask::IdentifyStartHandler(Identify *)
{
	LOG_INF("Identify start");
}

void AppTask::IdentifyStopHandler(Identify *)
{
	LOG_INF("Identify stop");
}

CHIP_ERROR AppTask::Init()
{
	/* Initialize and start the Matter server. */
	ReturnErrorOnFailure(Nrf::Matter::PrepareServer());
	ReturnErrorOnFailure(Nrf::Matter::StartServer());

	/* Initialize IR driver (PWM-based 38kHz carrier on P0.03). */
	int ret = ir_driver_init();
	if (ret) {
		LOG_ERR("IR driver init failed: %d", ret);
	}

	/* Initialize SCD40 sensor (I2C0, 30s polling). */
	ret = scd40_manager_init();
	if (ret) {
		LOG_ERR("SCD40 init failed: %d (sensor may not be connected)", ret);
	}

	/* Enable IR transmission now that init is complete.
	 * This prevents MatterPostAttributeChangeCallback from firing
	 * IR during Matter stack attribute restoration at boot. */
	zcl_callbacks_ready();

	return CHIP_NO_ERROR;
}

CHIP_ERROR AppTask::StartApp()
{
	ReturnErrorOnFailure(Init());

	while (true) {
		Nrf::DispatchNextTask();
	}

	return CHIP_NO_ERROR;
}
