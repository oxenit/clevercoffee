/**
 * @file pressureSensor.h
 *
 * @brief Pressure sensor support, selectable via hardware.sensors.pressure.type:
 *        - Type 0: Honeywell ABP2, taken from sample code from datasheet, adapted parameters for model ABP2LANT010BG2A3XX
 *        - Type 1: Analog transducer (5 VDC supply, 0.5-4.5 V ratiometric output) read via an ADS1115 ADC.
 *          The ADS1115 is powered at 5 V next to the transducer; its I2C lines must be connected to the ESP32
 *          through a bidirectional logic level converter (5 V <-> 3.3 V) on SDA and SCL.
 */

#pragma once

#include "Logger.h"
#include <Wire.h>

#define ABP2_READ_DELAY_MS (10)

enum PressureSensorType {
    PRESSURE_SENSOR_ABP2 = 0,
    PRESSURE_SENSOR_ADS1115 = 1,
};

inline int pressureSensorType = PRESSURE_SENSOR_ABP2;

constexpr unsigned long intervalPressureDebug = 1000;
inline unsigned long previousMillisPressureDebug = 0;

inline uint8_t ABP2_id = 0x28;                   // i2c address
inline uint8_t ABP2_data[7];                     // holds output data
inline uint8_t ABP2_cmd[3] = {0xAA, 0x00, 0x00}; // command to be sent
inline double ABP2_press_counts = 0.0;           // digital pressure reading [counts]
inline double ABP2_temp_counts = 0.0;            // digital temperature reading [counts]
inline double ABP2_pressure = -1.0;              // pressure reading [bar, psi, kPa, etc.]
inline double ABP2_temperature = 0.0;            // temperature reading in deg C
inline double ABP2_outputmax = 15099494.0;       // output at maximum pressure [counts]
inline double ABP2_outputmin = 1677722.0;        // output at minimum pressure [counts]
inline double ABP2_pmax = 10.0;                  // maximum value of pressure range [bar, psi, kPa, etc.]
inline double ABP2_pmin = 0.0;                   // minimum value of pressure range [bar, psi, kPa, etc.]
inline double ABP2_percentage = 0.0;             // holds percentage of full scale data

inline uint8_t ADS1115_id = 0x48;             // i2c address (ADDR pin to GND)
inline double ADS1115_vmin = 0.5;             // transducer output at minimum pressure [V]
inline double ADS1115_vmax = 4.5;             // transducer output at maximum pressure [V]
inline double ADS1115_pmin = 0.0;             // minimum value of pressure range [bar]
inline double ADS1115_pmax = 12.0;            // maximum value of pressure range [bar], adapt to the connected transducer
inline double ADS1115_pressure = -1.0;        // last valid pressure reading [bar]
constexpr double ADS1115_lsb = 6.144 / 32768; // volts per count at the +/-6.144 V PGA setting

// Conversion started once in initPressureSensor(): AIN0 vs GND, PGA +/-6.144 V (needed to cover the 4.5 V
// full scale output), continuous mode, 128 SPS, comparator disabled
constexpr uint16_t ADS1115_config = 0x4083;

inline float measurePressureABP2() {
    static uint8_t errorCount = 0;

    Wire.beginTransmission(ABP2_id);
    uint8_t stat = Wire.write(ABP2_cmd, 3); // write command to the sensor

    if (stat != 3) {
        LOG(ERROR, "Write Error");

        return static_cast<float>(ABP2_pressure);
    }

    stat = Wire.endTransmission();

    if (stat > 0) {
        if (const unsigned long currentMillisPressureDebug = millis(); currentMillisPressureDebug - previousMillisPressureDebug >= intervalPressureDebug && errorCount < 10) {
            LOGF(ERROR, "Could not communicate with pressure sensor, error code %i", stat);
            previousMillisPressureDebug = currentMillisPressureDebug;
            errorCount++;
        }

        return static_cast<float>(ABP2_pressure);
    }

    errorCount = 0;

    delay(ABP2_READ_DELAY_MS);

    // read back Sensor data 7 bytes
    Wire.requestFrom(ABP2_id, static_cast<uint8_t>(7));

    for (unsigned char& i : ABP2_data) {
        i = Wire.read();
    }

    // calculate digital pressure counts
    ABP2_press_counts = static_cast<double>(ABP2_data[3] + ABP2_data[2] * 256 + ABP2_data[1] * 65536);

    // calculate digital temperature counts
    ABP2_temp_counts = static_cast<double>(ABP2_data[6] + ABP2_data[5] * 256 + ABP2_data[4] * 65536);

    // calculate temperature in deg c
    ABP2_temperature = ABP2_temp_counts * 270.0 / 16777215.0 - 40.0;

    // calculate pressure as percentage of full scale
    ABP2_percentage = ABP2_press_counts / 16777215.0 * 100.0;

    // calculation of pressure value according to equation 2 of datasheet
    ABP2_pressure = (ABP2_press_counts - ABP2_outputmin) * (ABP2_pmax - ABP2_pmin) / (ABP2_outputmax - ABP2_outputmin) + ABP2_pmin;

    if (const unsigned long currentMillisPressureDebug = millis(); currentMillisPressureDebug - previousMillisPressureDebug >= intervalPressureDebug) {
        LOGF(TRACE, "Counts: %f, Percent: %f, Pressure: %f, Temp: %f", ABP2_press_counts, ABP2_percentage, ABP2_pressure, ABP2_temperature);
        previousMillisPressureDebug = currentMillisPressureDebug;
    }

    return static_cast<float>(ABP2_pressure);
}

inline float measurePressureADS1115() {
    static uint8_t errorCount = 0;

    // point to the conversion register, the ADC converts continuously in the background
    Wire.beginTransmission(ADS1115_id);
    Wire.write(0x00);

    if (const uint8_t stat = Wire.endTransmission(); stat > 0) {
        if (const unsigned long currentMillisPressureDebug = millis(); currentMillisPressureDebug - previousMillisPressureDebug >= intervalPressureDebug && errorCount < 10) {
            LOGF(ERROR, "Could not communicate with ADS1115, error code %i", stat);
            previousMillisPressureDebug = currentMillisPressureDebug;
            errorCount++;
        }

        return static_cast<float>(ADS1115_pressure);
    }

    if (Wire.requestFrom(ADS1115_id, static_cast<uint8_t>(2)) != 2) {
        return static_cast<float>(ADS1115_pressure);
    }

    errorCount = 0;

    const auto raw = static_cast<int16_t>((Wire.read() << 8) | Wire.read());
    const double voltage = raw * ADS1115_lsb;

    double pressure = (voltage - ADS1115_vmin) * (ADS1115_pmax - ADS1115_pmin) / (ADS1115_vmax - ADS1115_vmin) + ADS1115_pmin;

    // small negative readings occur at atmospheric pressure due to the 0.5 V offset tolerance
    if (pressure < ADS1115_pmin) {
        pressure = ADS1115_pmin;
    }

    ADS1115_pressure = pressure;

    if (const unsigned long currentMillisPressureDebug = millis(); currentMillisPressureDebug - previousMillisPressureDebug >= intervalPressureDebug) {
        LOGF(TRACE, "Raw: %i, Voltage: %f, Pressure: %f", raw, voltage, ADS1115_pressure);
        previousMillisPressureDebug = currentMillisPressureDebug;
    }

    return static_cast<float>(ADS1115_pressure);
}

inline void initPressureSensor(const int type) {
    pressureSensorType = type;

    if (pressureSensorType == PRESSURE_SENSOR_ADS1115) {
        Wire.beginTransmission(ADS1115_id);
        Wire.write(0x01); // config register
        Wire.write(static_cast<uint8_t>(ADS1115_config >> 8));
        Wire.write(static_cast<uint8_t>(ADS1115_config & 0xFF));

        if (const uint8_t stat = Wire.endTransmission(); stat > 0) {
            LOGF(ERROR, "Could not configure ADS1115, error code %i", stat);
        }
    }
}

inline float measurePressure() {
    if (pressureSensorType == PRESSURE_SENSOR_ADS1115) {
        return measurePressureADS1115();
    }

    return measurePressureABP2();
}
