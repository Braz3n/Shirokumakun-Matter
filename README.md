# Matter AC Controller (nRF Connect SDK)

Matter-over-Thread air conditioner controller for the Seeed XIAO nRF52840.
Transmits Hitachi Shirokuma-kun IR commands and reads CO2/temperature/humidity
from an SCD40 sensor.

## Endpoints

| EP | Device Type | Clusters |
|----|-------------|----------|
| 0  | Root Node   | System clusters |
| 1  | Thermostat + Fan | Thermostat, Fan Control |
| 2  | Humidity Sensor | Relative Humidity Measurement |
| 3  | Air Quality Sensor | Air Quality, CO2 Concentration |

## Hardware

- **MCU:** Seeed XIAO nRF52840 (BLE + 802.15.4)
- **IR LED:** on P0.03 (PWM0, 38 kHz carrier)
- **SCD40 sensor:** on P0.04 (SDA) / P0.05 (SCL), I2C0 at 100 kHz
- **Debug:** J-Link + RTT (USB disabled to save RAM)

## Prerequisites

### nRF Connect SDK v2.9.2

Install via [nRF Connect for VS Code](https://www.nordicsemi.com/Products/Development-tools/nRF-Connect-for-VS-Code)
or manually:

```bash
pip3 install west
mkdir ~/ncs && cd ~/ncs
west init -m https://github.com/nrfconnect/sdk-nrf --mr v2.9.2
west update
```

### Toolchain

The build requires the Zephyr SDK (ARM GCC cross-compiler, GN, etc.). The
easiest way to get it is via `nrfutil`:

```bash
# Install nrfutil if you don't have it
pip3 install nrfutil

# Install the toolchain bundle matching NCS v2.9.2
nrfutil toolchain-manager install --ncs-version v2.9.2 --install-dir ~/ncs/toolchains
```

Alternatively, install the [Zephyr SDK](https://github.com/zephyrproject-rtos/sdk-ng/releases)
manually (v0.16.1+) and ensure `arm-zephyr-eabi-gcc` and `gn` are on your PATH.

After installation, the toolchain lives at `~/ncs/toolchains/<hash>/`. The
build needs `gn` from this toolchain on PATH — see the Build section below.

### J-Link

Install [SEGGER J-Link](https://www.segger.com/downloads/jlink/) for flashing
and RTT debug output.

## Build

```bash
cd matter-ac-ncs

# Add GN to PATH (adjust toolchain hash if yours differs)
export PATH="$HOME/ncs/toolchains/7795df4459/opt/bin:$PATH"
export ZEPHYR_BASE="$HOME/ncs/zephyr"

west build -b xiao_ble/nrf52840
```

Use `-p always` for a pristine rebuild (required after devicetree or Kconfig
changes):

```bash
west build -b xiao_ble/nrf52840 -p always
```

## Flash

```bash
west flash --runner jlink
```

Or directly with J-Link Commander:

```bash
JLinkExe -device nRF52840_xxAA -if SWD -speed 4000 -autoconnect 1 \
  -CommandFile <(echo "loadfile build/matter-ac-ncs/zephyr/zephyr.hex; r; g; exit")
```

## RTT Logs

Start J-Link RTT Viewer or:

```bash
JLinkRTTClient
```

## Commission

Default pairing credentials (test values):

| Parameter | Value |
|-----------|-------|
| Discriminator | 0xF00 (3840) |
| Passcode | 20202021 |
| QR code | `MT:-24J042C00KA0648G00` |
| Manual code | `34970112332` |

Commission over BLE using Apple Home, Google Home, or chip-tool:

```bash
chip-tool pairing ble-thread <node-id> <thread-dataset> 20202021 3840
```

## ZAP Code Generation

The `src/zap-generated/` directory is checked in and only needs regeneration
if you modify `src/ac_controller.zap`:

```bash
ZAP_INSTALL_PATH=$HOME/ncs/modules/lib/matter/.zap/zap-v2024.08.14-nightly \
python3 $HOME/ncs/modules/lib/matter/scripts/tools/zap/generate.py \
  --no-prettify-output \
  -t $HOME/ncs/modules/lib/matter/src/app/zap-templates/app-templates.json \
  -z $HOME/ncs/modules/lib/matter/src/app/zap-templates/zcl/zcl.json \
  src/ac_controller.zap \
  -o src/zap-generated
```

## Project Structure

```
CMakeLists.txt               Build configuration
Kconfig                      OpenThread MTD defaults
prj.conf                     Zephyr/Matter Kconfig
sysbuild.conf                Sysbuild (Matter + no OTA)
boards/
  xiao_ble_nrf52840.overlay  Pin remapping, flash layout
src/
  main.cpp                   Entry point
  app_task.cpp/h             Matter init, task loop
  zcl_callbacks.cpp          Attribute change -> IR transmission
  ir_driver.cpp/h            nRF52840 PWM 38kHz carrier
  ir_protocol.cpp/h          Hitachi Shirokuma-kun encoding
  scd40_manager.cpp/h        SCD40 I2C driver, Matter attribute updates
  chip_project_config.h      CHIP project config (empty)
  ac_controller.zap          ZAP data model definition
  ac_controller.matter       Generated .matter IDL
  zap-generated/             Auto-generated cluster code
```
