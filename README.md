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
| 4  | Contact Sensor (dynamic) | Descriptor, Identify, Boolean State |

EP4 is a **dynamic endpoint** (no ZAP entry) representing IR ACK status — closed = OK, open = failed after retries.

## Adding a Dynamic Endpoint

Static endpoints are declared in `ac_controller.zap` and codegen'd into `zap-generated/`. Dynamic endpoints skip ZAP entirely and are registered at runtime with `emberAfSetDynamicEndpoint()`. Use this pattern when the endpoint count isn't fixed at build time, or when you want to keep a subsystem self-contained.

### Checklist

**1. `chip_project_config.h` — raise the dynamic endpoint count**
```cpp
#define CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT 1  // increment if adding more
```

**2. Declare attribute lists and cluster list**

Every endpoint needs the **Descriptor cluster** in its cluster list. Without it the commissioner can't enumerate device type or server clusters and the endpoint won't appear in Apple Home / chip-tool even though `emberAfSetDynamicEndpoint` logs success.

```cpp
// Descriptor — attributes are ARRAY; DescriptorAttrAccess (AttributeAccessInterface)
// serves the actual data automatically from the cluster list. Size 254 is conventional.
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(descriptorAttrs)
    DECLARE_DYNAMIC_ATTRIBUTE(Descriptor::Attributes::DeviceTypeList::Id, ARRAY, 254, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Descriptor::Attributes::ServerList::Id,     ARRAY, 254, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Descriptor::Attributes::ClientList::Id,     ARRAY, 254, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(Descriptor::Attributes::PartsList::Id,      ARRAY, 254, 0),
DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

// Per-cluster attribute lists — use ZAP_ATTRIBUTE_MASK(EXTERNAL_STORAGE) for each
// attribute so reads/writes are routed to emberAfExternalAttributeReadCallback /
// emberAfExternalAttributeWriteCallback. DECLARE_DYNAMIC_ATTRIBUTE_LIST_END()
// automatically appends ClusterRevision (0xFFFD) and FeatureMap (0xFFFC) as
// EXTERNAL_STORAGE, so handle those IDs in the read callback too.
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(myClusterAttrs)
    DECLARE_DYNAMIC_ATTRIBUTE(MyCluster::Attributes::Foo::Id, BOOLEAN, 1,
                              ZAP_ATTRIBUTE_MASK(EXTERNAL_STORAGE)),
DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(myClusters)
    DECLARE_DYNAMIC_CLUSTER(Descriptor::Id, descriptorAttrs,
                            ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(Identify::Id,   identifyAttrs,
                            ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(MyCluster::Id,  myClusterAttrs,
                            ZAP_CLUSTER_MASK(SERVER), nullptr, nullptr),
DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_ENDPOINT(myEndpoint, myClusters);
```

**3. DataVersion array — one entry per cluster**
```cpp
// Must match the number of clusters in myClusters (including Descriptor + Identify)
static DataVersion gDataVersions[3];
```
Getting this count wrong causes silent stack corruption. Count the `DECLARE_DYNAMIC_CLUSTER` lines.

**4. Implement `emberAfExternalAttributeReadCallback`**

The SDK provides weak no-op defaults that return `Failure`. Override them in your `.cpp`:
```cpp
chip::Protocols::InteractionModel::Status
emberAfExternalAttributeReadCallback(chip::EndpointId endpoint,
                                     chip::ClusterId clusterId,
                                     const EmberAfAttributeMetadata *attributeMetadata,
                                     uint8_t *buffer, uint16_t maxReadLength)
{
    if (endpoint != MY_EP) return Status::Failure;
    chip::AttributeId attrId = attributeMetadata->attributeId;

    // ClusterRevision / FeatureMap appended by DECLARE_DYNAMIC_ATTRIBUTE_LIST_END()
    if (attrId == 0xFFFD) { uint16_t r = MY_REVISION; memcpy(buffer, &r, 2); return Status::Success; }
    if (attrId == 0xFFFC) { uint32_t f = 0;           memcpy(buffer, &f, 4); return Status::Success; }

    // ... your cluster attributes ...
    return Status::Failure;
}
```
The `emberAfExternalAttributeReadCallback` / `emberAfExternalAttributeWriteCallback` are **global** — if you have multiple dynamic endpoints, gate on `endpoint` first. The Descriptor cluster attributes are **not** routed here (they go through `DescriptorAttrAccess` directly), so you don't need to handle cluster 0x001D.

**5. Register after `StartServer()`**
```cpp
// Must be called after Nrf::Matter::StartServer(). Lock the stack.
PlatformMgr().LockChipStack();
CHIP_ERROR err = emberAfSetDynamicEndpoint(
    0,          // slot index (0-based, up to CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT-1)
    MY_EP_ID,   // endpoint id (must not conflict with static endpoints)
    &myEndpoint,
    Span<DataVersion>(gDataVersions),
    Span<const EmberAfDeviceType>(kMyDeviceType));
PlatformMgr().UnlockChipStack();
```

**6. Updating attributes from application code**

Use the cluster accessor, not direct RAM writes:
```cpp
PlatformMgr().LockChipStack();
MyCluster::Attributes::Foo::Set(MY_EP_ID, value);
PlatformMgr().UnlockChipStack();
```
This routes through `emberAfExternalAttributeWriteCallback`, updates your backing variable, and automatically triggers subscription reports to controllers.

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
make flash
```

Or erase + flash (use for first-time programming):

```bash
make flash-erase
```

## DFU over USB

Once the device is running, firmware can be updated over USB without a J-Link using
[mcumgr-client](https://github.com/vouch-opensource/mcumgr-client):

```bash
make dfu
```

This uploads `build/matter-ac-ncs/zephyr/zephyr.signed.bin` to the device via SMP over `/dev/ttyACM1`.

### Zephyr CDC ACM bug

The Zephyr CDC ACM driver has a bug where the internal RX buffer is sized to
`CONFIG_CDC_ACM_BULK_EP_MPS` (64 bytes on full-speed USB) instead of 512, which
causes SMP transfers to stall. Apply this one-line fix to the NCS Zephyr source before
building (suggested by the mcumgr-client README):

```
~/ncs/zephyr/subsys/usb/device/class/cdc_acm.c line 74:
-  #define CDC_ACM_BUFFER_SIZE (CONFIG_CDC_ACM_BULK_EP_MPS)
+  #define CDC_ACM_BUFFER_SIZE 512
```

This fix will need to be reapplied after `west update`.

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
  pdm_manager.cpp/h          PDM mic, 2kHz Goertzel, dynamic EP4 (Contact Sensor)
  chip_project_config.h      CHIP project config (dynamic endpoint count)
  ac_controller.zap          ZAP data model definition
  ac_controller.matter       Generated .matter IDL
  zap-generated/             Auto-generated cluster code
```
