/**
 * @file pumpControl.cpp
 *
 * @brief Zero crossing ISR for the pump dimmer PSM control, see pumpControl.h
 */

#include "pumpControl.h"

void IRAM_ATTR pumpZeroCrossIsr() {
    // micros() is IRAM safe on the ESP32 Arduino core
    const uint32_t now = micros();

    if (now - pumpLastZeroCrossUs < pumpZeroCrossDebounceUs) {
        return;
    }

    pumpLastZeroCrossUs = now;

    if (!pumpPsmActive) {
        return;
    }

    // only decide at the start of a full mains cycle, the gate level is kept for both half-cycles
    pumpPsmHalfCycle = !pumpPsmHalfCycle;

    if (pumpPsmHalfCycle == false) {
        return;
    }

    uint8_t accumulator = pumpPsmAccumulator + pumpPsmDuty;
    const bool conduct = accumulator >= 100;

    if (conduct) {
        accumulator -= 100;
    }

    // direct register access, digitalWrite is not safe in an IRAM ISR
    if (conduct == pumpPsmOnIsHigh) {
        GPIO.out_w1ts = (1UL << PIN_PUMP);
    }
    else {
        GPIO.out_w1tc = (1UL << PIN_PUMP);
    }

    pumpPsmAccumulator = accumulator;
}
