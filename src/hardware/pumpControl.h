/**
 * @file pumpControl.h
 *
 * @brief Variable pump power through an AC dimmer module (e.g. RobotDyn type) using pulse skip modulation (PSM),
 *        with a pressure control loop for pressure profiling.
 *
 *        Wiring: the dimmer gate/PWM input replaces the pump SSR on PIN_PUMP, the dimmer zero crossing
 *        output goes to PIN_ZC. Power the dimmer logic side with 3.3 V: its zero crossing output is pulled
 *        up to its supply voltage and the ESP32 inputs are not 5 V tolerant.
 *
 *        PSM works on full mains cycles: on every second zero crossing the ISR decides whether the triac
 *        conducts for the whole upcoming cycle (Bresenham distribution of the requested duty). Vibratory pumps
 *        (ULKA, CEME) have a series diode and only take power from one polarity, so deciding per half-cycle
 *        would let the skip pattern lock onto a single polarity (e.g. 50 % duty -> either full power or none).
 *
 *        When PSM is inactive the ISR leaves the pump pin alone, so the existing on/off relay control
 *        (manual flush, backflush, hot water, non-profiled brews) keeps working unchanged: a constantly
 *        high gate simply means full power.
 */

#pragma once

#include "Logger.h"
#include "pinmapping.h"
#include <Arduino.h>
#include <soc/gpio_struct.h>

static_assert(PIN_PUMP < 32, "pumpZeroCrossIsr writes GPIO.out_w1ts/out_w1tc, which only cover GPIO 0-31");

// PI gains of the pressure control loop, in % duty per bar. Starting points for an ULKA vibratory pump,
// adjust if the pressure oscillates (lower) or reacts too slowly (raise)
constexpr float pumpPressureKp = 10.0f;
constexpr float pumpPressureKi = 25.0f;
constexpr float pumpControlIntervalSec = 0.1f; // update rate of the control loop, matches intervalPressure

// Consecutive failed pressure reads (at intervalPressure = 100 ms) after which a profiled brew falls back to full pump power
constexpr uint8_t pressureProfilingMaxSensorErrors = 5;

// Zero crossing edges closer together than this are treated as noise (a 60 Hz half-cycle lasts 8.3 ms)
constexpr uint32_t pumpZeroCrossDebounceUs = 4000;

inline volatile bool pumpPsmActive = false;  // when false the ISR does not touch the pump pin (relay control)
inline volatile uint8_t pumpPsmDuty = 0;     // requested pump power [0-100 %]
inline volatile uint8_t pumpPsmAccumulator = 0;
inline volatile bool pumpPsmHalfCycle = false; // true while in the second half of a mains cycle
inline volatile uint32_t pumpLastZeroCrossUs = 0;
inline volatile bool pumpPsmOnIsHigh = true; // follows the configured pump relay trigger type

inline float pumpPressureTarget = 0.0f; // pressure target of the control loop [bar]
inline float pumpPsmDutyFloat = 0.0f;
inline float pumpPressureLastError = 0.0f;
inline bool pumpPressureFirstUpdate = true;

// Set when the pressure sensor stops responding during a profiled brew; the brew then continues at full pump power
// (like a brew without profiling) instead of regulating on a stale reading. Cleared when the brew ends.
inline bool pumpProfilingFaulted = false;

// defined in pumpControl.cpp, an IRAM ISR cannot be inline in a header (literal placement)
void IRAM_ATTR pumpZeroCrossIsr();

inline void pumpControlInit(const bool onLevelIsHigh) {
    pumpPsmOnIsHigh = onLevelIsHigh;
    pinMode(PIN_ZC, INPUT);
    attachInterrupt(digitalPinToInterrupt(PIN_ZC), pumpZeroCrossIsr, RISING);
    LOG(INFO, "Pump dimmer control initialized (PSM on zero crossing signal)");
}

inline void startPumpProfiling(const float targetPressure) {
    pumpPressureTarget = targetPressure;

    // rough steady-state guess so the loop does not start from zero
    float initialDuty = 8.0f * targetPressure;

    if (initialDuty > 100.0f) {
        initialDuty = 100.0f;
    }

    pumpPsmDutyFloat = initialDuty;
    pumpPressureLastError = 0.0f;
    pumpPressureFirstUpdate = true;

    // the ISR is not running PSM yet, so these can be set without a critical section
    pumpPsmDuty = static_cast<uint8_t>(initialDuty);
    pumpPsmAccumulator = 0;
    pumpPsmHalfCycle = false;
    pumpPsmActive = true;

    LOGF(INFO, "Pressure profiling started, target %.1f bar", targetPressure);
}

inline void stopPumpProfiling() {
    if (pumpPsmActive) {
        LOG(INFO, "Pressure profiling stopped");
    }

    pumpPsmActive = false;
    pumpPsmDuty = 0;
    pumpPressureTarget = 0.0f;
}

/**
 * @brief PI control of the pump power towards pumpPressureTarget, velocity form so the duty itself
 *        is the integrator state and cannot wind up beyond its clamp. Call at a fixed rate of
 *        pumpControlIntervalSec whenever a new pressure reading is available.
 */
inline void updatePumpPressureControl(const float currentPressure) {
    if (!pumpPsmActive) {
        return;
    }

    const float error = pumpPressureTarget - currentPressure;

    // no proportional kick on the first sample: the error jumps from "unknown" to the full target
    // difference, which would otherwise add Kp * target on top of the initial duty guess
    if (pumpPressureFirstUpdate) {
        pumpPressureLastError = error;
        pumpPressureFirstUpdate = false;
    }

    pumpPsmDutyFloat += pumpPressureKp * (error - pumpPressureLastError) + pumpPressureKi * error * pumpControlIntervalSec;
    pumpPressureLastError = error;

    if (pumpPsmDutyFloat > 100.0f) {
        pumpPsmDutyFloat = 100.0f;
    }
    else if (pumpPsmDutyFloat < 0.0f) {
        pumpPsmDutyFloat = 0.0f;
    }

    pumpPsmDuty = static_cast<uint8_t>(pumpPsmDutyFloat + 0.5f);
}
