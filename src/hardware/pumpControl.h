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

// Pressure targets below this switch the pump off instead of regulating (bloom / pause phases of a profile)
constexpr float pumpPressureOffThreshold = 0.1f;

// Zero crossing edges closer together than this are treated as noise (a 60 Hz half-cycle lasts 8.3 ms)
constexpr uint32_t pumpZeroCrossDebounceUs = 4000;

inline volatile bool pumpPsmActive = false;    // when false the ISR does not touch the pump pin (relay control)
inline volatile uint8_t pumpPsmDuty = 0;       // requested pump power [0-100 %]
inline volatile uint8_t pumpPsmAccumulator = 0;
inline volatile bool pumpPsmHalfCycle = false; // true while in the second half of a mains cycle
inline volatile uint32_t pumpLastZeroCrossUs = 0;
inline volatile bool pumpPsmOnIsHigh = true;   // follows the configured pump relay trigger type

inline float pumpPressureTarget = 0.0f;        // pressure target of the control loop [bar]
inline float pumpPsmDutyFloat = 0.0f;
inline float pumpPressureLastError = 0.0f;
inline bool pumpPressureFirstUpdate = true;
inline bool pumpFixedPowerMode = false; // profile phase with a fixed pump power: no pressure regulation

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

/**
 * @brief Rough steady-state duty for a pressure target, used to seed the control loop
 */
inline float pumpInitialDutyGuess(const float targetPressure) {
    const float duty = 8.0f * targetPressure;
    return duty > 100.0f ? 100.0f : duty;
}

inline void startPumpProfiling(const float targetPressure) {
    pumpPressureTarget = targetPressure;

    // rough steady-state guess so the loop does not start from zero (0 for a pump off target)
    const float initialDuty = targetPressure < pumpPressureOffThreshold ? 0.0f : pumpInitialDutyGuess(targetPressure);

    pumpPsmDutyFloat = initialDuty;
    pumpPressureLastError = 0.0f;
    pumpPressureFirstUpdate = true;
    pumpFixedPowerMode = false;

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
    pumpFixedPowerMode = false;
}

/**
 * @brief Run the pump at a fixed power without pressure feedback (profile power phases). PSM must be active.
 */
inline void setPumpPower(float power) {
    if (power < 0.0f) {
        power = 0.0f;
    }
    else if (power > 100.0f) {
        power = 100.0f;
    }

    pumpFixedPowerMode = true;
    pumpPressureTarget = 0.0f;
    pumpPsmDutyFloat = power;
    pumpPsmDuty = static_cast<uint8_t>(power + 0.5f);
}

/**
 * @brief Move the pressure target of a running control loop (profile phases and ramps). Unlike
 *        startPumpProfiling this keeps the PI state, except when coming out of a pump off (0 bar)
 *        phase, where the loop is re-seeded as if it was starting fresh.
 */
inline void setPumpPressureTarget(const float targetPressure) {
    if (pumpFixedPowerMode) {
        // bumpless switch from a fixed power phase: the PI loop continues from the current power
        pumpFixedPowerMode = false;
        pumpPressureTarget = targetPressure;
        pumpPressureFirstUpdate = true;

        if (targetPressure < pumpPressureOffThreshold || pumpPsmDutyFloat > 0.0f) {
            return;
        }
        // coming from 0 % power: seed like a fresh start below
        pumpPsmDutyFloat = pumpInitialDutyGuess(targetPressure);
        pumpPsmDuty = static_cast<uint8_t>(pumpPsmDutyFloat);
        return;
    }

    const bool wasOff = pumpPressureTarget < pumpPressureOffThreshold;
    pumpPressureTarget = targetPressure;

    if (wasOff && targetPressure >= pumpPressureOffThreshold) {
        pumpPsmDutyFloat = pumpInitialDutyGuess(targetPressure);
        pumpPsmDuty = static_cast<uint8_t>(pumpPsmDutyFloat);
        pumpPressureFirstUpdate = true;
    }
}

/**
 * @brief PI control of the pump power towards pumpPressureTarget, velocity form so the duty itself
 *        is the integrator state and cannot wind up beyond its clamp. Call at a fixed rate of
 *        pumpControlIntervalSec whenever a new pressure reading is available.
 */
inline void updatePumpPressureControl(const float currentPressure) {
    if (!pumpPsmActive || pumpFixedPowerMode) {
        return;
    }

    // pump off phase (bloom / pause): no regulation, the valve stays open
    if (pumpPressureTarget < pumpPressureOffThreshold) {
        pumpPsmDutyFloat = 0.0f;
        pumpPsmDuty = 0;
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
