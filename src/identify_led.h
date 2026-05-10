#pragma once

#ifdef __cplusplus
#include <app/clusters/identify-server/IdentifyCluster.h>
chip::app::Clusters::IdentifyDelegate & identify_led_delegate();
extern "C" {
#endif

/* Start a 5-second 2Hz blue blink (auto-stops). Used by the shell command. */
void identify_led_trigger();

#ifdef __cplusplus
}
#endif
