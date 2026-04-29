/*
 * PDM microphone manager — on-demand 2kHz ACK detector.
 *
 * Hardware: MSM261D3526HICPM-C PDM microphone on Seeed XIAO nRF52840 Sense
 *   PDM CLK:    P1.00  (pinctrl pdm0_default in xiao_ble-pinctrl.dtsi)
 *   PDM DIN:    P0.16  (pinctrl pdm0_default in xiao_ble-pinctrl.dtsi)
 *   Mic Enable: P1.10  active HIGH — must be driven manually
 *
 * Algorithm: Goertzel for f=2000Hz, fs≈16125Hz (1.032MHz PDM / 64), N=1024
 *   coeff = 2*cos(2*pi*2000/16125) ≈ 1.42288  (exact frequency, N-independent)
 *
 * Matter: dynamic endpoint 4, Contact Sensor (device type 0x0015),
 *         Boolean State cluster — StateValue true = "Open" (failed).
 */

#include "pdm_manager.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/irq.h>

#include <nrfx_pdm.h>
#include <hal/nrf_gpio.h>

#include <app/util/attribute-storage.h>
#include <platform/CHIPDeviceLayer.h>

LOG_MODULE_REGISTER(pdm_mgr, LOG_LEVEL_INF);

/* --------------------------------------------------------------------------
 * Hardware constants
 * -------------------------------------------------------------------------- */

#define PDM_CLK_PIN   NRF_GPIO_PIN_MAP(1, 0)   /* P1.00 */
#define PDM_DIN_PIN   NRF_GPIO_PIN_MAP(0, 16)  /* P0.16 */
#define MIC_EN_PORT   1
#define MIC_EN_PIN    10                        /* P1.10 active HIGH */

/* --------------------------------------------------------------------------
 * Detection constants
 * -------------------------------------------------------------------------- */

#define PDM_BUF_SAMPLES   1024
#define GOERTZEL_COEFF    1.42288f      /* 2*cos(2*pi*2000/16125) exact 2kHz, N-independent */
#define DETECT_THRESHOLD  0.1f          /* normalized power — tune empirically */

/* --------------------------------------------------------------------------
 * State
 * -------------------------------------------------------------------------- */

static int16_t pdm_buf[2][PDM_BUF_SAMPLES];
static volatile uint8_t buf_ready_idx;
static volatile bool    buf_available;

static struct k_work pdm_process_work;
static struct k_sem  ack_sem;
static volatile bool pdm_capture_active;

/* nrfx_pdm instance (nrfx >= 3.7.0 multi-instance API) */
static const nrfx_pdm_t pdm_inst = NRFX_PDM_INSTANCE(0);

/* --------------------------------------------------------------------------
 * Matter dynamic endpoint descriptors
 * -------------------------------------------------------------------------- */

using namespace chip;
using namespace chip::app;
using namespace chip::app::Clusters;
using namespace chip::DeviceLayer;

/* Contact Sensor device type 0x0015, revision 1 */
static const EmberAfDeviceType kContactSensorDeviceType[] = {{ 0x0015, 1 }};
static DataVersion gAlarmDataVersions[1];

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(boolStateAttrs)
    DECLARE_DYNAMIC_ATTRIBUTE(BooleanState::Attributes::StateValue::Id, BOOLEAN, 1, 0),
DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(alarmClusters)
    DECLARE_DYNAMIC_CLUSTER(BooleanState::Id, boolStateAttrs,
                            ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_ENDPOINT(alarmEndpoint, alarmClusters);

/* --------------------------------------------------------------------------
 * Goertzel tone detector — runs in system workqueue
 * -------------------------------------------------------------------------- */

static bool goertzel_detect_2khz(const int16_t *samples, int n)
{
    float s1 = 0.0f, s2 = 0.0f;

    for (int i = 0; i < n; i++) {
        float s = (float)samples[i] + GOERTZEL_COEFF * s1 - s2;
        s2 = s1;
        s1 = s;
    }

    float power = s2 * s2 + s1 * s1 - GOERTZEL_COEFF * s1 * s2;
    /* Normalize by (N * full_scale)^2 so result is independent of N */
    float norm = power / ((float)n * 32768.0f * (float)n * 32768.0f);

    LOG_DBG("Goertzel (x1e6): %.4f", (double)(norm * 1e6f));

    return norm > DETECT_THRESHOLD;
}

/* --------------------------------------------------------------------------
 * Work handler — runs in system workqueue
 * -------------------------------------------------------------------------- */

static void pdm_process_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    if (!pdm_capture_active || !buf_available) {
        return;
    }

    /* Snapshot index under brief critical section to match ISR write */
    unsigned int key = irq_lock();
    uint8_t idx = buf_ready_idx;
    buf_available = false;
    irq_unlock(key);

    bool tone_present = goertzel_detect_2khz(pdm_buf[idx], PDM_BUF_SAMPLES);

    if (tone_present) {
        LOG_INF("PDM: ACK detected");
        k_sem_give(&ack_sem);
    }
}

/* --------------------------------------------------------------------------
 * nrfx_pdm ISR callback — ISR context, keep minimal
 * -------------------------------------------------------------------------- */

static void pdm_event_handler(const nrfx_pdm_evt_t *evt)
{
    static uint8_t next = 0;

    if (evt->buffer_requested) {
        nrfx_pdm_buffer_set(&pdm_inst, pdm_buf[next], PDM_BUF_SAMPLES);
        next ^= 1;
    }

    if (evt->buffer_released != NULL) {
        buf_ready_idx = (evt->buffer_released == pdm_buf[0]) ? 0 : 1;
        buf_available = true;
        k_work_submit(&pdm_process_work);
    }
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void pdm_manager_start_listen(void)
{
    k_sem_reset(&ack_sem);
    buf_available = false;
    pdm_capture_active = true;

    nrfx_err_t err = nrfx_pdm_start(&pdm_inst);
    if (err != NRFX_SUCCESS) {
        LOG_ERR("PDM: nrfx_pdm_start failed: 0x%x", err);
        pdm_capture_active = false;
    }
}

bool pdm_manager_collect_ack(uint32_t timeout_ms)
{
    int rc = k_sem_take(&ack_sem, K_MSEC(timeout_ms));

    pdm_capture_active = false;

    nrfx_err_t err = nrfx_pdm_stop(&pdm_inst);
    if (err != NRFX_SUCCESS && err != NRFX_ERROR_INVALID_STATE) {
        LOG_WRN("PDM: nrfx_pdm_stop unexpected error: 0x%x", err);
    }

    return (rc == 0);
}

void pdm_manager_init(void)
{
    /* --- 1. Enable microphone via P1.10 HIGH --- */
    const struct device *gpio1 = DEVICE_DT_GET(DT_NODELABEL(gpio1));

    if (!device_is_ready(gpio1)) {
        LOG_ERR("PDM: GPIO1 device not ready");
        return;
    }

    int ret = gpio_pin_configure(gpio1, MIC_EN_PIN, GPIO_OUTPUT_ACTIVE);
    if (ret) {
        LOG_ERR("PDM: Mic enable GPIO config failed: %d", ret);
        return;
    }

    gpio_pin_set(gpio1, MIC_EN_PIN, 1);
    LOG_INF("PDM: Mic enable (P1.10) HIGH");

    /* --- 2. Wire nrfx_pdm IRQ ---
     * Use IRQ_PRIO_LOWEST: Zephyr's nrfx glue ignores nrfx interrupt_priority
     * (NRFX_IRQ_PRIORITY_SET is a no-op); the only place priority is set is here. */
    IRQ_CONNECT(PDM_IRQn, IRQ_PRIO_LOWEST, nrfx_isr, nrfx_pdm_0_irq_handler, 0);
    irq_enable(PDM_IRQn);

    /* --- 3. Init nrfx_pdm --- */
    nrfx_pdm_config_t cfg = NRFX_PDM_DEFAULT_CONFIG(PDM_CLK_PIN, PDM_DIN_PIN);
    cfg.gain_l = NRF_PDM_GAIN_MAXIMUM;
    cfg.gain_r = NRF_PDM_GAIN_MAXIMUM;

    nrfx_err_t err = nrfx_pdm_init(&pdm_inst, &cfg, pdm_event_handler);
    if (err != NRFX_SUCCESS) {
        LOG_ERR("PDM: nrfx_pdm_init failed: 0x%x", err);
        return;
    }

    /* --- 4. Init work item and semaphore --- */
    k_work_init(&pdm_process_work, pdm_process_work_handler);
    k_sem_init(&ack_sem, 0, 1);

    /* --- 5. Register dynamic Matter endpoint (chip stack must be started) --- */
    PlatformMgr().LockChipStack();

    CHIP_ERROR chip_err = emberAfSetDynamicEndpoint(
        0,            /* dynamic endpoint slot index */
        4,            /* endpoint id */
        &alarmEndpoint,
        Span<DataVersion>(gAlarmDataVersions),
        Span<const EmberAfDeviceType>(kContactSensorDeviceType));

    PlatformMgr().UnlockChipStack();

    if (chip_err != CHIP_NO_ERROR) {
        LOG_ERR("PDM: Dynamic endpoint registration failed: %" CHIP_ERROR_FORMAT,
                chip_err.Format());
        return;
    }

    LOG_INF("PDM: Dynamic endpoint 4 registered (Contact Sensor / Boolean State)");
    LOG_INF("PDM: Initialized — capture starts on demand");
}
