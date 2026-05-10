/*
 * SCD40 CO2/Temperature/Humidity sensor manager.
 *
 * Raw I2C driver for Sensirion SCD40:
 *   - 0x21B1: start periodic measurement
 *   - 0xE4B8: data ready status
 *   - 0xEC05: read measurement (9 bytes: CO2/Temp/RH with CRCs)
 *   - 0x3F86: stop periodic measurement
 *
 * Updates Matter attributes on:
 *   EP1: Thermostat::LocalTemperature, TemperatureMeasurement::MeasuredValue
 *   EP2: RelativeHumidityMeasurement::MeasuredValue
 *   EP3: AirQuality, CarbonDioxideConcentrationMeasurement::MeasuredValue
 */

#include "scd40_manager.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app/clusters/thermostat-server/thermostat-server.h>
#include <app/clusters/air-quality-server/air-quality-server.h>
#include <app/clusters/concentration-measurement-server/concentration-measurement-server.h>
#include <platform/CHIPDeviceLayer.h>

LOG_MODULE_REGISTER(scd40, LOG_LEVEL_INF);

#define SCD40_I2C_ADDR 0x62

/* SCD40 commands (big-endian 16-bit) */
#define SCD40_CMD_START_PERIODIC   0x21B1
#define SCD40_CMD_DATA_READY       0xE4B8
#define SCD40_CMD_READ_MEASUREMENT 0xEC05
#define SCD40_CMD_STOP_PERIODIC    0x3F86

#define POLL_INTERVAL_SEC 15

static const struct device *i2c_dev;
static struct k_timer       poll_timer;
static struct k_work        poll_work;
static struct Scd40Reading  last_reading;

using namespace chip;
using namespace chip::app::Clusters;

/* Air Quality cluster server instance (EP3). */
static AirQuality::Instance
    air_quality_instance(3, /* endpointId */
                         BitMask<AirQuality::Feature, uint32_t>(
                             AirQuality::Feature::kFair, AirQuality::Feature::kModerate,
                             AirQuality::Feature::kVeryPoor, AirQuality::Feature::kExtremelyPoor));

/* CO2 concentration measurement instance (EP3).
 * Template: <NumericMeasurement, LevelIndication, MediumLevel,
 *            CriticalLevel, PeakMeasurement, AverageMeasurement> */
static ConcentrationMeasurement::Instance<true, false, false, false, false, false>
    co2_instance(3, /* endpointId */
                 CarbonDioxideConcentrationMeasurement::Id,
                 ConcentrationMeasurement::MeasurementMediumEnum::kAir,
                 ConcentrationMeasurement::MeasurementUnitEnum::kPpm);

/* CRC-8 for Sensirion (polynomial 0x31, init 0xFF). */
static uint8_t sensirion_crc(const uint8_t *data, uint8_t len) {
    uint8_t crc = 0xFF;

    for (uint8_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
        }
    }
    return crc;
}

static int scd40_write_cmd(uint16_t cmd) {
    uint8_t buf[2] = {(uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFF)};
    int     ret;

    /* SCD40 DNACKs command bytes while busy measuring (~5s cycle).
     * Retry for up to 6 seconds to cover a full measurement period. */
    for (int attempt = 0; attempt < 60; attempt++) {
        ret = i2c_write(i2c_dev, buf, sizeof(buf), SCD40_I2C_ADDR);
        if (ret == 0) {
            if (attempt > 0) {
                LOG_INF("write 0x%04X succeeded on attempt %d", cmd, attempt);
            }
            return 0;
        }
        k_sleep(K_MSEC(100));
    }

    return ret;
}

static int scd40_read_measurement(uint16_t *co2_ppm, int16_t *temp_001c, uint16_t *rh_001pct) {
    int     ret;
    uint8_t buf[9];

    ret = scd40_write_cmd(SCD40_CMD_READ_MEASUREMENT);
    if (ret) {
        return ret;
    }

    /* SCD40 needs ~1ms to prepare the response */
    k_sleep(K_MSEC(1));

    ret = i2c_read(i2c_dev, buf, sizeof(buf), SCD40_I2C_ADDR);
    if (ret) {
        return ret;
    }

    /* Verify CRCs for each 2-byte word */
    for (int i = 0; i < 9; i += 3) {
        if (sensirion_crc(&buf[i], 2) != buf[i + 2]) {
            LOG_WRN("SCD40 CRC mismatch at byte %d", i);
            return -EIO;
        }
    }

    /* CO2 in ppm (raw value IS the ppm) */
    *co2_ppm = ((uint16_t)buf[0] << 8) | buf[1];

    /* Temperature: T [degC] = -45 + 175 * (raw / 65535)
     * We want 0.01 degC units for Matter. */
    uint16_t raw_temp  = ((uint16_t)buf[3] << 8) | buf[4];
    int32_t  temp_x100 = -4500 + (int32_t)(17500UL * raw_temp / 65535UL);
    *temp_001c         = (int16_t)temp_x100;

    /* Humidity: RH [%] = 100 * (raw / 65535)
     * We want 0.01% units for Matter (0-10000). */
    uint16_t raw_rh = ((uint16_t)buf[6] << 8) | buf[7];
    *rh_001pct      = (uint16_t)(10000UL * raw_rh / 65535UL);

    return 0;
}

/* Map CO2 ppm to Matter AirQuality enum. */
static AirQuality::AirQualityEnum co2_to_air_quality(uint16_t co2_ppm) {
    if (co2_ppm < 400)
        return AirQuality::AirQualityEnum::kGood;
    if (co2_ppm < 600)
        return AirQuality::AirQualityEnum::kFair;
    if (co2_ppm < 1000)
        return AirQuality::AirQualityEnum::kModerate;
    if (co2_ppm < 1500)
        return AirQuality::AirQualityEnum::kPoor;
    if (co2_ppm < 2000)
        return AirQuality::AirQualityEnum::kVeryPoor;
    return AirQuality::AirQualityEnum::kExtremelyPoor;
}

static void update_matter_attributes(uint16_t co2_ppm, int16_t temp_001c, uint16_t rh_001pct) {
    using namespace chip::DeviceLayer;

    PlatformMgr().LockChipStack();

    /* EP1: Thermostat — LocalTemperature (0.01 C, nullable int16s) */
    Thermostat::Attributes::LocalTemperature::Set(1, temp_001c);

    /* EP2: RelativeHumidityMeasurement — MeasuredValue (0.01%, uint16) */
    RelativeHumidityMeasurement::Attributes::MeasuredValue::Set(2, rh_001pct);

    /* EP3: AirQuality */
    air_quality_instance.UpdateAirQuality(co2_to_air_quality(co2_ppm));

    /* EP3: CO2 concentration (float, ppm) */
    co2_instance.SetMeasuredValue(chip::app::DataModel::MakeNullable(static_cast<float>(co2_ppm)));

    PlatformMgr().UnlockChipStack();

    last_reading = {co2_ppm, temp_001c, rh_001pct, true};
}

struct Scd40Reading scd40_manager_get_last_reading(void) {
    return last_reading;
}

static void poll_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    int ret;

    /* Check data ready */
    ret = scd40_write_cmd(SCD40_CMD_DATA_READY);
    if (ret) {
        LOG_WRN("SCD40 data_ready cmd failed: %d", ret);
        return;
    }

    k_sleep(K_MSEC(1));

    uint8_t ready_buf[3];

    ret = i2c_read(i2c_dev, ready_buf, sizeof(ready_buf), SCD40_I2C_ADDR);
    if (ret) {
        LOG_WRN("SCD40 data_ready read failed: %d", ret);
        return;
    }

    uint16_t ready = ((uint16_t)ready_buf[0] << 8) | ready_buf[1];

    if ((ready & 0x07FF) == 0) {
        /* Data not ready yet */
        return;
    }

    uint16_t co2_ppm;
    int16_t  temp_001c;
    uint16_t rh_001pct;

    ret = scd40_read_measurement(&co2_ppm, &temp_001c, &rh_001pct);
    if (ret) {
        LOG_WRN("SCD40 read_measurement failed: %d", ret);
        return;
    }

    update_matter_attributes(co2_ppm, temp_001c, rh_001pct);
}

static void poll_timer_handler(struct k_timer *timer) {
    ARG_UNUSED(timer);
    k_work_submit(&poll_work);
}

int scd40_manager_init(void) {
    i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c0));
    if (!device_is_ready(i2c_dev)) {
        LOG_ERR("I2C device not ready");
        return -ENODEV;
    }

    /* Initialize Air Quality and CO2 cluster server instances */
    air_quality_instance.Init();
    co2_instance.Init();
    co2_instance.SetMinMeasuredValue(chip::app::DataModel::MakeNullable(0.0f));
    co2_instance.SetMaxMeasuredValue(chip::app::DataModel::MakeNullable(40000.0f));

    /* SCD40 needs ~1000ms after power-up before responding to I2C */
    k_sleep(K_MSEC(1000));

    /* Stop any in-progress periodic measurement (e.g. from previous firmware) */
    scd40_write_cmd(SCD40_CMD_STOP_PERIODIC);
    k_sleep(K_MSEC(500));

    /* Start periodic measurement (one reading every ~5s internally) */
    int ret = scd40_write_cmd(SCD40_CMD_START_PERIODIC);
    if (ret) {
        LOG_ERR("SCD40 start_periodic failed: %d", ret);
        return ret;
    }

    LOG_INF("SCD40 periodic measurement started");

    /* Set up polling */
    k_work_init(&poll_work, poll_work_handler);
    k_timer_init(&poll_timer, poll_timer_handler, NULL);
    k_timer_start(&poll_timer, K_SECONDS(POLL_INTERVAL_SEC), K_SECONDS(POLL_INTERVAL_SEC));

    return 0;
}
