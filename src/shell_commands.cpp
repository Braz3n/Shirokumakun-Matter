/*
 * Shell commands for the AC controller — all under the `ac` parent command.
 *
 * ac reset     — factory reset (clear fabrics) and reboot
 * ac qr        — reprint QR code and manual pairing code
 * ac goertzel  — stream Goertzel 2kHz power readings on|off
 * ac threshold — get or set ACK detection threshold
 */

#include <zephyr/shell/shell.h>
#include <app/server/Server.h>
#include <app/server/OnboardingCodesUtil.h>
#include <platform/CHIPDeviceLayer.h>
#include "pdm_manager.h"
#include "ir_driver.h"
#include "ir_protocol.h"

using namespace chip;
using namespace chip::DeviceLayer;

/* ac reset — clear all fabrics and reboot into unpaired state */
static int cmd_reset(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    shell_warn(sh, "Clearing pairing state and rebooting...");
    PlatformMgr().LockChipStack();
    Server::GetInstance().ScheduleFactoryReset();
    PlatformMgr().UnlockChipStack();
    return 0;
}

/* ac qr — reprint QR code and manual pairing code */
static int cmd_qr(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(sh);
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    /* PrintOnboardingCodes logs via ChipLogProgress — appears on all log backends */
    PrintOnboardingCodes(RendezvousInformationFlags(RendezvousInformationFlag::kBLE));
    return 0;
}

/* ac goertzel on|off — stream Goertzel power via LOG_INF (~64ms per sample) */
static int cmd_goertzel(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2) {
        shell_help(sh);
        return 0;
    }
    if (strcmp(argv[1], "on") == 0) {
        pdm_manager_verbose_start();
        shell_print(sh, "Goertzel verbose ON  (threshold x1e6: %.1f)",
                    (double)(pdm_manager_get_threshold() * 1e6f));
    } else if (strcmp(argv[1], "off") == 0) {
        pdm_manager_verbose_stop();
        shell_print(sh, "Goertzel verbose OFF");
    } else {
        shell_error(sh, "usage: ac goertzel on|off");
        return -EINVAL;
    }
    return 0;
}

/* ac threshold [value] — get or set detection threshold (normalized power; x1e6 units shown) */
static int cmd_threshold(const struct shell *sh, size_t argc, char **argv)
{
    if (argc == 1) {
        shell_print(sh, "%.6f  (x1e6: %.1f)",
                    (double)pdm_manager_get_threshold(),
                    (double)(pdm_manager_get_threshold() * 1e6f));
    } else {
        float t = (float)atof(argv[1]);
        if (t <= 0.0f) {
            shell_error(sh, "threshold must be > 0");
            return -EINVAL;
        }
        pdm_manager_set_threshold(t);
        shell_print(sh, "Set to %.6f", (double)t);
    }
    return 0;
}

/* ac off — send power-off IR command immediately */
static int cmd_off(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    static struct IrPulse pulses[IR_MAX_PULSES];
    struct AcState state = { .power = false, .mode = AC_MODE_COOLING,
                             .temp_c = 24, .fan = FAN_SPEED_AUTO };
    uint16_t count = ac_encode_pulses(&state, pulses);
    ir_dispatch_command(pulses, count);
    shell_print(sh, "AC off command queued");
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_ac,
    SHELL_CMD_ARG(off,        NULL, "Send power-off IR command",            cmd_off,       1, 0),
    SHELL_CMD_ARG(reset,     NULL, "Clear pairing state (factory reset)", cmd_reset,     1, 0),
    SHELL_CMD_ARG(qr,        NULL, "Print QR code and manual pairing code", cmd_qr,      1, 0),
    SHELL_CMD_ARG(goertzel,  NULL, "Stream Goertzel power on|off",        cmd_goertzel,  2, 0),
    SHELL_CMD_ARG(threshold, NULL, "Get/set ACK detect threshold [value]", cmd_threshold, 1, 1),
    SHELL_SUBCMD_SET_END
);
static int cmd_ac(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    shell_print(sh, "Subcommands:");
    shell_print(sh, "  off        Send power-off IR command");
    shell_print(sh, "  reset      Clear pairing state (factory reset)");
    shell_print(sh, "  qr         Print QR code and manual pairing code");
    shell_print(sh, "  goertzel   Stream Goertzel 2kHz power  on|off");
    shell_print(sh, "  threshold  Get/set ACK detection threshold [value]");
    return 0;
}

SHELL_CMD_REGISTER(ac, &sub_ac, "AC controller commands", cmd_ac);
