/*
 * Shell commands.
 *
 * ac       — AC controller: off, scd, cfar, threshold
 * matter   — Matter pairing:  reset, qr
 * reboot   — warm reboot (preserves pairing state)
 */

#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>
#include <qrcodegen.h>
#include <app/server/Server.h>
#include <setup_payload/OnboardingCodesUtil.h>
#include <platform/CHIPDeviceLayer.h>
#include "pdm_manager.h"
#include "scd40_manager.h"
#include "ir_driver.h"
#include "ir_protocol.h"

using namespace chip;
using namespace chip::DeviceLayer;

/* -------------------------------------------------------------------------
 * ac commands
 * ------------------------------------------------------------------------- */

/* ac off — send power-off IR command immediately */
static int cmd_off(const struct shell *sh, size_t argc, char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    static struct IrPulse pulses[IR_MAX_PULSES];
    struct AcState        state = {
               .power = false, .mode = AC_MODE_COOLING, .temp_c = 24, .fan = FAN_SPEED_AUTO};
    uint16_t count = ac_encode_pulses(&state, pulses);
    ir_dispatch_command(pulses, count);
    shell_print(sh, "AC off command queued");
    return 0;
}

/* ac scd — print last SCD40 reading */
static int cmd_scd(const struct shell *sh, size_t argc, char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    struct Scd40Reading r = scd40_manager_get_last_reading();
    if (!r.valid) {
        shell_warn(sh, "No reading yet");
        return 0;
    }
    int16_t t_abs = r.temp_001c >= 0 ? r.temp_001c : -r.temp_001c;
    shell_print(sh, "CO2: %u ppm  T: %s%d.%02d C  RH: %u.%02u %%",
                r.co2_ppm,
                r.temp_001c < 0 ? "-" : "", t_abs / 100, t_abs % 100,
                r.rh_001pct / 100, r.rh_001pct % 100);
    return 0;
}

/* ac cfar on|off — stream CFAR power via LOG_INF (~64ms per sample) */
static int cmd_cfar(const struct shell *sh, size_t argc, char **argv) {
    if (argc < 2) {
        shell_help(sh);
        return 0;
    }
    if (strcmp(argv[1], "on") == 0) {
        pdm_manager_verbose_start();
        shell_print(sh, "CFAR verbose ON  (multiplier: %.1fx)",
                    (double)pdm_manager_get_threshold());
    } else if (strcmp(argv[1], "off") == 0) {
        pdm_manager_verbose_stop();
        shell_print(sh, "CFAR verbose OFF");
    } else {
        shell_error(sh, "usage: ac cfar on|off");
        return -EINVAL;
    }
    return 0;
}

/* ac threshold [value] — get or set CFAR multiplier (N× above noise floor) */
static int cmd_threshold(const struct shell *sh, size_t argc, char **argv) {
    if (argc == 1) {
        shell_print(sh, "%.1f", (double)pdm_manager_get_threshold());
    } else {
        float t = (float)atof(argv[1]);
        if (t <= 0.0f) {
            shell_error(sh, "threshold must be > 0");
            return -EINVAL;
        }
        pdm_manager_save_threshold(t);
        shell_print(sh, "Set and saved to %.6f", (double)t);
    }
    return 0;
}

static int cmd_ac(const struct shell *sh, size_t argc, char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    shell_help(sh);
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_ac,
    SHELL_CMD_ARG(off,       NULL, "Send power-off IR command",            cmd_off,       1, 0),
    SHELL_CMD_ARG(scd,       NULL, "Print last SCD40 reading",             cmd_scd,       1, 0),
    SHELL_CMD_ARG(cfar,      NULL, "Stream CFAR power readings  on|off",   cmd_cfar,      2, 0),
    SHELL_CMD_ARG(threshold, NULL, "Get/set ACK detection threshold [val]",cmd_threshold, 1, 1),
    SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(ac, &sub_ac, "AC controller commands", cmd_ac);

/* -------------------------------------------------------------------------
 * matter commands
 * ------------------------------------------------------------------------- */

/* matter reset — clear all fabrics and reboot into unpaired state */
static int cmd_reset(const struct shell *sh, size_t argc, char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    shell_warn(sh, "Clearing pairing state and rebooting...");
    PlatformMgr().LockChipStack();
    Server::GetInstance().ScheduleFactoryReset();
    PlatformMgr().UnlockChipStack();
    return 0;
}

/* matter qr — render QR code as ASCII art and print manual pairing code */
static int cmd_qr(const struct shell *sh, size_t argc, char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    char qr_str[64] = {};
    chip::MutableCharSpan qr_span(qr_str, sizeof(qr_str) - 1);
    if (GetQRCode(qr_span, RendezvousInformationFlags(RendezvousInformationFlag::kBLE))
            != CHIP_NO_ERROR) {
        shell_error(sh, "Failed to get QR code");
        return -EIO;
    }
    qr_str[qr_span.size()] = '\0';

    char manual_str[32] = {};
    chip::MutableCharSpan manual_span(manual_str, sizeof(manual_str) - 1);
    if (GetManualPairingCode(manual_span,
            RendezvousInformationFlags(RendezvousInformationFlag::kBLE))
            == CHIP_NO_ERROR) {
        manual_str[manual_span.size()] = '\0';
    }

    static uint8_t qr_buf[qrcodegen_BUFFER_LEN_FOR_VERSION(9)];
    static uint8_t qr_tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(9)];
    if (!qrcodegen_encodeText(qr_str, qr_tmp, qr_buf,
                              qrcodegen_Ecc_LOW, 1, 9,
                              qrcodegen_Mask_AUTO, true)) {
        shell_error(sh, "QR encode failed");
        return -ENOMEM;
    }

    /* Render: dark module → "  " (terminal bg), light module → "##" (terminal fg).
     * Reads correctly on dark-background terminals; most scanners handle the inversion. */
    int size  = qrcodegen_getSize(qr_buf);
    int quiet = 2;
    char line[256];

    for (int y = -quiet; y < size + quiet; y++) {
        int pos = 0;
        for (int x = -quiet; x < size + quiet; x++) {
            bool dark = (x >= 0 && x < size && y >= 0 && y < size)
                        && qrcodegen_getModule(qr_buf, x, y);
            line[pos++] = dark ? ' ' : '#';
            line[pos++] = dark ? ' ' : '#';
        }
        line[pos] = '\0';
        shell_print(sh, "%s", line);
    }

    shell_print(sh, "");
    shell_print(sh, "QR:     %s", qr_str);
    shell_print(sh, "Manual: %s", manual_str);

    return 0;
}

static int cmd_matter(const struct shell *sh, size_t argc, char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    shell_help(sh);
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_matter,
    SHELL_CMD_ARG(reset, NULL, "Factory reset (clears all pairing data)", cmd_reset, 1, 0),
    SHELL_CMD_ARG(qr,    NULL, "Print QR code and manual pairing code",   cmd_qr,    1, 0),
    SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(matter, &sub_matter, "Matter pairing commands", cmd_matter);

/* -------------------------------------------------------------------------
 * reboot — top-level
 * ------------------------------------------------------------------------- */

static int cmd_reboot(const struct shell *sh, size_t argc, char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    shell_warn(sh, "Rebooting...");
    sys_reboot(SYS_REBOOT_WARM);
    return 0;
}

SHELL_CMD_REGISTER(reboot, NULL, "Warm reboot (preserves pairing state)", cmd_reboot);
