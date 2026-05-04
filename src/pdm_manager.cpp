/*
 * PDM microphone manager — on-demand 2kHz ACK detector.
 *
 * Hardware: MSM261D3526HICPM-C PDM microphone on Seeed XIAO nRF52840 Sense
 *   PDM CLK:    P1.00  (pinctrl pdm0_default in xiao_ble-pinctrl.dtsi)
 *   PDM DIN:    P0.16  (pinctrl pdm0_default in xiao_ble-pinctrl.dtsi)
 *   Mic Enable: P1.10  active HIGH — must be driven manually
 *
 * Algorithm: 1024-point real FFT (CMSIS-DSP arm_rfft_fast_f32) with Hanning window.
 *   fs ≈ 16125 Hz (1.032 MHz PDM / 64), bin spacing ≈ 15.75 Hz, target bin 127 ≈ 2000 Hz.
 *   CA-CFAR detection: CUT power vs. mean of training cells on either side of target bin.
 */

#include "pdm_manager.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/irq.h>
#include <zephyr/settings/settings.h>

#include <nrfx_pdm.h>
#include <hal/nrf_gpio.h>
#include <arm_math.h>
#include <math.h>
#include <cstdlib>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

LOG_MODULE_REGISTER(pdm_mgr, LOG_LEVEL_INF);

/* --------------------------------------------------------------------------
 * Hardware constants
 * -------------------------------------------------------------------------- */

#define PDM_CLK_PIN NRF_GPIO_PIN_MAP(1, 0)  /* P1.00 */
#define PDM_DIN_PIN NRF_GPIO_PIN_MAP(0, 16) /* P0.16 */
#define MIC_EN_PORT 1
#define MIC_EN_PIN  10 /* P1.10 active HIGH */

/* --------------------------------------------------------------------------
 * Detection constants
 * -------------------------------------------------------------------------- */

#define PDM_BUF_SAMPLES      1024
#define PDM_TARGET_BIN       127       /* round(2000 * 1024 / 16125) — 2000 Hz */
#define PDM_CFAR_GUARD_CELLS 2         /* bins each side excluded from noise estimate */
#define PDM_CFAR_TRAIN_CELLS 10        /* bins each side used for noise estimate */
#define PDM_SCALING_FACTOR   1e-6f     /* scale applied before logging */
static float detect_threshold = 25.0f; /* CFAR multiplier: N× above noise estimate */

static int pdm_settings_set(const char *name, size_t len,
                             settings_read_cb read_cb, void *cb_arg)
{
    if (strcmp(name, "threshold") == 0 && len == sizeof(float)) {
        read_cb(cb_arg, &detect_threshold, sizeof(float));
    }
    return 0;
}
SETTINGS_STATIC_HANDLER_DEFINE(pdm, "pdm", NULL, pdm_settings_set, NULL, NULL);

/* --------------------------------------------------------------------------
 * State
 * -------------------------------------------------------------------------- */

static int16_t          pdm_buf[2][PDM_BUF_SAMPLES];
static volatile uint8_t buf_ready_idx;
static volatile bool    buf_available;

static struct k_work pdm_process_work;
static struct k_sem  ack_sem;
static volatile bool pdm_capture_active;
static volatile bool pdm_verbose     = false;
static volatile bool pdm_verbose_own = false;

static arm_rfft_fast_instance_f32 fft_inst;
static float                      fft_in[PDM_BUF_SAMPLES];
static float                      fft_out[PDM_BUF_SAMPLES];
static float                      hann_window[PDM_BUF_SAMPLES];

static nrfx_pdm_t pdm_inst = NRFX_PDM_INSTANCE(NRF_PDM0);

/* --------------------------------------------------------------------------
 * Work handler — FFT + CA-CFAR, runs in system workqueue
 * -------------------------------------------------------------------------- */

static void pdm_process_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (!buf_available) {
        return;
    }

    unsigned int key = irq_lock();
    uint8_t      idx = buf_ready_idx;
    buf_available    = false;
    irq_unlock(key);

    /* Apply Hanning window and convert to float */
    for (int i = 0; i < PDM_BUF_SAMPLES; i++) {
        fft_in[i] = (float)pdm_buf[idx][i] * hann_window[i];
    }

    arm_rfft_fast_f32(&fft_inst, fft_in, fft_out, 0 /* forward */);

    /* CUT: magnitude squared at target bin.
     * arm_rfft_fast_f32 packs output as [Re0, ReN/2, Re1, Im1, Re2, Im2, ...] */
    float re  = fft_out[2 * PDM_TARGET_BIN];
    float im  = fft_out[2 * PDM_TARGET_BIN + 1];
    float cut = re * re + im * im;

    /* CA-CFAR: average training cells on both sides, skipping guard cells */
    float noise_sum = 0.0f;
    for (int k = PDM_TARGET_BIN - PDM_CFAR_GUARD_CELLS - PDM_CFAR_TRAIN_CELLS;
         k <= PDM_TARGET_BIN + PDM_CFAR_GUARD_CELLS + PDM_CFAR_TRAIN_CELLS; k++) {
        if (abs(k - PDM_TARGET_BIN) > PDM_CFAR_GUARD_CELLS) {
            float r = fft_out[2 * k], im_k = fft_out[2 * k + 1];
            noise_sum += r * r + im_k * im_k;
        }
    }
    float noise_est   = noise_sum / (2 * PDM_CFAR_TRAIN_CELLS);
    float cfar_thresh = noise_est * detect_threshold;

    LOG_DBG("PDM cut (x1e-6): %.2f  noise_est (x1e-6): %.2f", (double)(cut * PDM_SCALING_FACTOR),
            (double)(noise_est * PDM_SCALING_FACTOR));

    if (pdm_verbose) {
        LOG_INF("PDM cut (x1e-6): %.2f  thresh (x1e-6): %.2f%s", (double)(cut * PDM_SCALING_FACTOR),
                (double)(cfar_thresh * PDM_SCALING_FACTOR),
                cut > cfar_thresh ? "  [ABOVE THRESHOLD]" : "");
    }

    if (pdm_capture_active && cut > cfar_thresh) {
        LOG_INF("PDM: ACK detected (cut=%.2f thresh=%.2f)", (double)(cut * PDM_SCALING_FACTOR),
                (double)(cfar_thresh * PDM_SCALING_FACTOR));
        k_sem_give(&ack_sem);
    }
}

/* --------------------------------------------------------------------------
 * nrfx_pdm ISR callback — ISR context, keep minimal
 * -------------------------------------------------------------------------- */

static void pdm_event_handler(const nrfx_pdm_evt_t *evt) {
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

void pdm_manager_start_listen(void) {
    k_sem_reset(&ack_sem);
    buf_available      = false;
    pdm_capture_active = true;

    int err = nrfx_pdm_start(&pdm_inst);
    if (err < 0) {
        LOG_ERR("PDM: nrfx_pdm_start failed: %d", err);
        pdm_capture_active = false;
    }
}

bool pdm_manager_collect_ack(uint32_t timeout_ms) {
    int rc = k_sem_take(&ack_sem, K_MSEC(timeout_ms));

    pdm_capture_active = false;

    int err = nrfx_pdm_stop(&pdm_inst);
    if (err < 0) {
        LOG_WRN("PDM: nrfx_pdm_stop unexpected error: %d", err);
    }

    return (rc == 0);
}

void pdm_manager_init(void) {
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
    IRQ_CONNECT(PDM_IRQn, IRQ_PRIO_LOWEST, nrfx_pdm_irq_handler, &pdm_inst, 0);
    irq_enable(PDM_IRQn);

    /* --- 3. Init nrfx_pdm --- */
    nrfx_pdm_config_t cfg = NRFX_PDM_DEFAULT_CONFIG(PDM_CLK_PIN, PDM_DIN_PIN);
    cfg.gain_l            = NRF_PDM_GAIN_MAXIMUM;
    cfg.gain_r            = NRF_PDM_GAIN_MAXIMUM;

    int err = nrfx_pdm_init(&pdm_inst, &cfg, pdm_event_handler);
    if (err < 0) {
        LOG_ERR("PDM: nrfx_pdm_init failed: %d", err);
        return;
    }

    /* --- 4. Precompute Hanning window and init FFT --- */
    for (int i = 0; i < PDM_BUF_SAMPLES; i++) {
        hann_window[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (PDM_BUF_SAMPLES - 1)));
    }
    arm_rfft_fast_init_1024_f32(&fft_inst);

    /* --- 5. Init work item and semaphore --- */
    k_work_init(&pdm_process_work, pdm_process_work_handler);
    k_sem_init(&ack_sem, 0, 1);

    /* --- 6. Load persisted settings (threshold) --- */
    int settings_err = settings_load_subtree("pdm");
    if (settings_err) {
        LOG_WRN("PDM: settings load failed: %d (using default threshold)", settings_err);
    } else {
        LOG_INF("PDM: threshold loaded: %.2f", (double)detect_threshold);
    }

    LOG_INF("PDM: Initialized — capture starts on demand");
}

void pdm_manager_verbose_start(void) {
    pdm_verbose = true;
    if (!pdm_capture_active) {
        int err = nrfx_pdm_start(&pdm_inst);
        if (err < 0) {
            LOG_ERR("PDM: verbose start failed: %d", err);
        } else {
            pdm_verbose_own = true;
        }
    }
    /* if pdm_capture_active, PDM is already running — verbose piggybacks */
}

void pdm_manager_verbose_stop(void) {
    pdm_verbose = false;
    if (pdm_verbose_own) {
        nrfx_pdm_stop(&pdm_inst);
        pdm_verbose_own = false;
    }
}

void pdm_manager_set_threshold(float t) {
    detect_threshold = t;
}
void pdm_manager_save_threshold(float t) {
    detect_threshold = t;
    int err = settings_save_one("pdm/threshold", &t, sizeof(float));
    if (err) {
        LOG_ERR("PDM: threshold save failed: %d", err);
    }
}
float pdm_manager_get_threshold(void) {
    return detect_threshold;
}

