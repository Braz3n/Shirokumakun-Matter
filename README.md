# Matter AC Controller (nRF Connect SDK)

Matter-over-Thread air conditioner controller for the Seeed XIAO nRF52840 Sense.
Transmits Hitachi Shirokuma-kun IR commands and reads CO2/temperature/humidity
from an SCD40 sensor.

## Endpoints

| EP | Device Type        | Clusters                          |
|----|--------------------|-----------------------------------|
| 0  | Root Node          | System clusters                   |
| 1  | Thermostat + Fan   | Thermostat, Fan Control           |
| 2  | Humidity Sensor    | Relative Humidity Measurement     |
| 3  | Air Quality Sensor | Air Quality, CO2 Concentration    |

## Shell Commands

Connect to `/dev/ttyACM0` (USB-CDC, any baud — DTR-gated, so open a terminal to activate):

```bash
screen /dev/ttyACM0
```

| Command                | Description                                                        |
|------------------------|--------------------------------------------------------------------|
| `ac off`               | Send power-off IR command immediately                              |
| `ac scd`               | Print the most recent SCD40 reading (CO2, temperature, humidity)   |
| `ac cfar on\|off`      | Stream CFAR power readings for threshold tuning                    |
| `ac threshold [value]` | Get or set the CFAR detection threshold (persisted across reboots) |
| `matter reset`         | Factory reset — clears all pairing data (including threshold) and reboots |
| `matter qr`            | Reprint QR code and manual pairing code                            |
| `reboot`               | Warm reboot, preserving Matter pairing state                       |

The CFAR threshold is stored in NVS and restored on boot. `matter reset` wipes it back to the default of 25×.

## Hardware

- **MCU:**    Seeed XIAO nRF52840 Sense (BLE + 802.15.4)
- **IR LED:** P0.02 (PWM0, 38 kHz carrier)
- **SCD40:**  P0.04 (SDA) / P0.05 (SCL), I2C0 at 100 kHz
- **PDM mic:** P1.00 (CLK) / P0.16 (DIN), enable P1.10 active HIGH
- **Shell:**  USB-CDC ACM0 (logs + interactive shell)
- **DFU:**    USB-CDC ACM1 (SMP/mcumgr)
- **Flash:**  J-Link SWD (initial programming)

## Prerequisites

- **Docker** — all builds run inside the container; no local toolchain needed.
- **J-Link** — [SEGGER J-Link](https://www.segger.com/downloads/jlink/) for initial flashing.
- **mcumgr-client** — [mcumgr-client](https://github.com/vouch-opensource/mcumgr-client) for DFU over USB.

### Setup (once)

#### Option A — VS Code devcontainer

Open the repo in VS Code and choose **Reopen in Container**. The image builds automatically and the NCS workspace is initialised on first launch.

#### Option B — Docker CLI

```bash
make image   # build the builder image
make init    # download NCS v3.3.0 into a Docker volume
```

## Build

```bash
make build      # incremental build
make pristine   # clean rebuild (required after devicetree or Kconfig changes)
```

## Flash

Initial programming via J-Link (erases the entire device including settings):

```bash
make flash
```

## DFU over USB

Once running, firmware can be updated over USB without a J-Link:

```bash
make dfu
```

Uploads `build/zephyr/zephyr.signed.bin` via SMP over ACM1, marks it for test, and resets.

> **Zephyr CDC ACM bug:** The ACM RX buffer is sized to 64 bytes instead of 512, stalling SMP transfers. Fix it before building:
> ```
> ncs/zephyr/subsys/usb/device/class/cdc_acm.c line 74:
> -  #define CDC_ACM_BUFFER_SIZE (CONFIG_CDC_ACM_BULK_EP_MPS)
> +  #define CDC_ACM_BUFFER_SIZE 512
> ```
> Reapply after `west update`.

## Commission

Run `matter qr` on the shell to print the QR code and manual pairing code (unique per device, derived from the hardware ID).

Commission over BLE using Apple Home, Google Home, or chip-tool:

```bash
chip-tool pairing ble-thread <node-id> <thread-dataset> <passcode> <discriminator>
```

## ZAP Code Generation

`src/zap-generated/` is checked in and only needs regeneration when `src/ac_controller.zap` changes:

```bash
ZAP_INSTALL_PATH=$HOME/ncs/modules/lib/matter/.zap/zap-v2024.08.14-nightly \
python3 $HOME/ncs/modules/lib/matter/scripts/tools/zap/generate.py \
  --no-prettify-output \
  -t $HOME/ncs/modules/lib/matter/src/app/zap-templates/app-templates.json \
  -z $HOME/ncs/modules/lib/matter/src/app/zap-templates/zcl/zcl.json \
  src/ac_controller.zap -o src/zap-generated
```

## Project Structure

```
CMakeLists.txt          Build configuration
prj.conf                Zephyr/Matter Kconfig
Makefile                Docker build, flash, and DFU targets
boards/                 Device tree overlays and partition map
src/
  app_task.cpp/h        Matter init, task loop
  zcl_callbacks.cpp     Attribute change → IR transmission
  hw_pairing.cpp/h      FICR-derived discriminator + SPAKE2+ passcode
  shell_commands.cpp    USB-CDC shell (ac *)
  ir_driver.cpp/h       nRF52840 PWM 38 kHz carrier
  ir_protocol.cpp/h     Hitachi Shirokuma-kun encoding
  scd40_manager.cpp/h   SCD40 I2C driver + Matter attribute updates
  pdm_manager.cpp/h     PDM mic, 2 kHz FFT + CA-CFAR, IR ACK detection
  ac_controller.zap     ZAP data model definition
  zap-generated/        Auto-generated cluster code
```
