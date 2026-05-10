/*
 * Identify cluster feedback: 2 Hz blue LED blink.
 *
 * Implements IdentifyDelegate for the new ServerClusterInterface model used
 * in NCS v3.3.0 / CHIP SDK 2.9.x.  The LED blinks while the Matter Identify
 * timer counts down (OnIdentifyStart / OnIdentifyStop) and also when triggered
 * directly by the "matter identify" shell command (identify_led_trigger).
 */

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

#include <app/clusters/identify-server/IdentifyCluster.h>
#include "identify_led.h"

using namespace chip::app::Clusters;

/* Blue LED: P0.30, active-low (common anode RGB on XIAO nRF52840). */
static const struct gpio_dt_spec led_blue = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);

static struct k_timer blink_timer;
static struct k_timer stop_timer;   /* auto-stop for shell-triggered blink */
static struct k_work  blink_work;
static bool           blink_on;

static void blink_work_fn(struct k_work *)
{
    blink_on = !blink_on;
    gpio_pin_set_dt(&led_blue, (int)blink_on);
}

static void blink_timer_fn(struct k_timer *)
{
    k_work_submit(&blink_work);
}

static void stop_timer_fn(struct k_timer *)
{
    k_timer_stop(&blink_timer);
    gpio_pin_set_dt(&led_blue, 0);
}

static void start_blink()
{
    k_timer_stop(&stop_timer);
    blink_on = false;
    k_timer_start(&blink_timer, K_MSEC(250), K_MSEC(250));
}

static void stop_blink()
{
    k_timer_stop(&stop_timer);
    k_timer_stop(&blink_timer);
    gpio_pin_set_dt(&led_blue, 0);
}

class LedIdentifyDelegate : public IdentifyDelegate
{
public:
    void OnIdentifyStart(IdentifyCluster &) override { start_blink(); }
    void OnIdentifyStop(IdentifyCluster &) override  { stop_blink(); }
    void OnTriggerEffect(IdentifyCluster &) override { start_blink(); }
    bool IsTriggerEffectEnabled() const override     { return true; }
};

static LedIdentifyDelegate gDelegate;

IdentifyDelegate & identify_led_delegate()
{
    return gDelegate;
}

void identify_led_trigger()
{
    start_blink();
    k_timer_start(&stop_timer, K_SECONDS(5), K_NO_WAIT);
}

static int identify_led_init(void)
{
    if (!gpio_is_ready_dt(&led_blue)) {
        return -ENODEV;
    }
    gpio_pin_configure_dt(&led_blue, GPIO_OUTPUT_INACTIVE);
    k_work_init(&blink_work, blink_work_fn);
    k_timer_init(&blink_timer, blink_timer_fn, NULL);
    k_timer_init(&stop_timer, stop_timer_fn, NULL);
    return 0;
}

SYS_INIT(identify_led_init, APPLICATION, 91);
