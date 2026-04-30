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
 *
 * Dynamic endpoint attribute storage:
 *   All cluster-specific attributes are EXTERNAL_STORAGE.  Reads and writes
 *   are served by emberAfExternalAttributeReadCallback /
 *   emberAfExternalAttributeWriteCallback below.  Global attributes
 *   (ClusterRevision, FeatureMap) are added as EXTERNAL_STORAGE by
 *   DECLARE_DYNAMIC_ATTRIBUTE_LIST_END() and handled the same way.
 *
 *   ir_driver.cpp updates StateValue via BooleanState::Attributes::StateValue::Set(4, v),
 *   which routes through the write callback and then triggers subscription reports.
 */

#include "pdm_manager.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/irq.h>
#include <cstring>

#include <nrfx_pdm.h>
#include <hal/nrf_gpio.h>

#include <app/util/attribute-storage.h>
#include <platform/CHIPDeviceLayer.h>
#include <protocols/interaction_model/StatusCode.h>

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
static float detect_threshold = 0.1f;  /* normalized power — tune empirically */

/* --------------------------------------------------------------------------
 * State
 * -------------------------------------------------------------------------- */

static int16_t pdm_buf[2][PDM_BUF_SAMPLES];
static volatile uint8_t buf_ready_idx;
static volatile bool    buf_available;

static struct k_work pdm_process_work;
static struct k_sem  ack_sem;
static volatile bool pdm_capture_active;
static volatile bool goertzel_verbose         = false;
static volatile bool goertzel_verbose_pdm_own = false;

/* Backing store for BooleanState::StateValue on EP4.
 * Written via emberAfExternalAttributeWriteCallback from Set(4, ...) in ir_driver.cpp. */
static bool ep4_state_value = false;

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
static DataVersion gAlarmDataVersions[3]; /* one per cluster: Descriptor + Identify + BooleanState */

/* All cluster-specific attributes are EXTERNAL_STORAGE — no dynamic RAM backing.
 * DECLARE_DYNAMIC_ATTRIBUTE_LIST_END() appends ClusterRevision + FeatureMap,
 * also as EXTERNAL_STORAGE, automatically. */

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(descriptorAttrs)
    DECLARE_DYNAMIC_ATTRIBUTE(Descriptor::Attributes::DeviceTypeList::Id, ARRAY, 254, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Descriptor::Attributes::ServerList::Id,     ARRAY, 254, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Descriptor::Attributes::ClientList::Id,     ARRAY, 254, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Descriptor::Attributes::PartsList::Id,      ARRAY, 254, 0),
DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(identifyAttrs)
    DECLARE_DYNAMIC_ATTRIBUTE(Identify::Attributes::IdentifyTime::Id, INT16U, 2,
                              ZAP_ATTRIBUTE_MASK(EXTERNAL_STORAGE) | ZAP_ATTRIBUTE_MASK(WRITABLE)),
    DECLARE_DYNAMIC_ATTRIBUTE(Identify::Attributes::IdentifyType::Id, ENUM8, 1,
                              ZAP_ATTRIBUTE_MASK(EXTERNAL_STORAGE)),
DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(boolStateAttrs)
    DECLARE_DYNAMIC_ATTRIBUTE(BooleanState::Attributes::StateValue::Id, BOOLEAN, 1,
                              ZAP_ATTRIBUTE_MASK(EXTERNAL_STORAGE)),
DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(alarmClusters)
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs,
                            ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id, identifyAttrs,
                            ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(BooleanState::Id, boolStateAttrs,
                            ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_ENDPOINT(alarmEndpoint, alarmClusters);

/* --------------------------------------------------------------------------
 * External attribute callbacks — serve all EP4 attributes from local state.
 * These override the weak defaults in the CHIP SDK (which return FAILURE).
 * -------------------------------------------------------------------------- */

using chip::Protocols::InteractionModel::Status;

chip::Protocols::InteractionModel::Status
emberAfExternalAttributeReadCallback(chip::EndpointId endpoint,
                                     chip::ClusterId clusterId,
                                     const EmberAfAttributeMetadata *attributeMetadata,
                                     uint8_t *buffer,
                                     uint16_t maxReadLength)
{
    if (endpoint != 4) {
        return Status::Failure;
    }

    chip::AttributeId attrId = attributeMetadata->attributeId;

    /* Global attributes appended by DECLARE_DYNAMIC_ATTRIBUTE_LIST_END() */
    if (attrId == 0xFFFD) { /* ClusterRevision */
        /* Identify cluster revision 4 (Matter 1.0); BooleanState revision 1 */
        uint16_t rev = (clusterId == Identify::Id) ? 4u : 1u;
        std::memcpy(buffer, &rev, sizeof(rev));
        return Status::Success;
    }
    if (attrId == 0xFFFC) { /* FeatureMap */
        uint32_t fm = 0;
        std::memcpy(buffer, &fm, sizeof(fm));
        return Status::Success;
    }

    /* Identify cluster — static no-op responses */
    if (clusterId == Identify::Id) {
        if (attrId == Identify::Attributes::IdentifyTime::Id) {
            uint16_t val = 0;
            std::memcpy(buffer, &val, sizeof(val));
            return Status::Success;
        }
        if (attrId == Identify::Attributes::IdentifyType::Id) {
            *buffer = static_cast<uint8_t>(Identify::IdentifyTypeEnum::kNone);
            return Status::Success;
        }
    }

    /* BooleanState cluster */
    if (clusterId == BooleanState::Id &&
        attrId == BooleanState::Attributes::StateValue::Id) {
        *buffer = ep4_state_value ? 1 : 0;
        return Status::Success;
    }

    return Status::Failure;
}

chip::Protocols::InteractionModel::Status
emberAfExternalAttributeWriteCallback(chip::EndpointId endpoint,
                                      chip::ClusterId clusterId,
                                      const EmberAfAttributeMetadata *attributeMetadata,
                                      uint8_t *buffer)
{
    if (endpoint != 4) {
        return Status::Failure;
    }

    chip::AttributeId attrId = attributeMetadata->attributeId;

    /* Accept IdentifyTime writes silently (no-op identify) */
    if (clusterId == Identify::Id &&
        attrId == Identify::Attributes::IdentifyTime::Id) {
        return Status::Success;
    }

    /* StateValue written by ir_driver.cpp via BooleanState::Attributes::StateValue::Set(4, v).
     * After this callback returns SUCCESS, the stack automatically triggers subscription reports. */
    if (clusterId == BooleanState::Id &&
        attrId == BooleanState::Attributes::StateValue::Id) {
        ep4_state_value = (*buffer != 0);
        return Status::Success;
    }

    return Status::Failure;
}

/* --------------------------------------------------------------------------
 * Goertzel tone detector — runs in system workqueue
 * -------------------------------------------------------------------------- */

static float goertzel_power(const int16_t *samples, int n)
{
    float s1 = 0.0f, s2 = 0.0f;

    for (int i = 0; i < n; i++) {
        float s = (float)samples[i] + GOERTZEL_COEFF * s1 - s2;
        s2 = s1;
        s1 = s;
    }

    float power = s2 * s2 + s1 * s1 - GOERTZEL_COEFF * s1 * s2;
    /* Normalize by (N * full_scale)^2 so result is independent of N */
    return power / ((float)n * 32768.0f * (float)n * 32768.0f);
}

/* --------------------------------------------------------------------------
 * Work handler — runs in system workqueue
 * -------------------------------------------------------------------------- */

static void pdm_process_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    if (!buf_available) {
        return;
    }

    /* Snapshot index under brief critical section to match ISR write */
    unsigned int key = irq_lock();
    uint8_t idx = buf_ready_idx;
    buf_available = false;
    irq_unlock(key);

    float norm = goertzel_power(pdm_buf[idx], PDM_BUF_SAMPLES);
    LOG_DBG("Goertzel (x1e6): %.4f", (double)(norm * 1e6f));

    if (goertzel_verbose) {
        LOG_INF("PDM power x1e6: %.1f%s", (double)(norm * 1e6f),
                norm > detect_threshold ? "  [ABOVE THRESHOLD]" : "");
    }

    if (pdm_capture_active && norm > detect_threshold) {
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

void pdm_manager_verbose_start(void)
{
    goertzel_verbose = true;
    if (!pdm_capture_active) {
        nrfx_err_t err = nrfx_pdm_start(&pdm_inst);
        if (err == NRFX_SUCCESS) {
            goertzel_verbose_pdm_own = true;
        } else {
            LOG_ERR("PDM: verbose start failed: 0x%x", err);
        }
    }
    /* if pdm_capture_active, PDM is already running — verbose piggybacks */
}

void pdm_manager_verbose_stop(void)
{
    goertzel_verbose = false;
    if (goertzel_verbose_pdm_own) {
        nrfx_pdm_stop(&pdm_inst);
        goertzel_verbose_pdm_own = false;
    }
}

void  pdm_manager_set_threshold(float t) { detect_threshold = t; }
float pdm_manager_get_threshold(void)    { return detect_threshold; }
