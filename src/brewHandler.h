/**
 * @file brewHandler.h
 *
 * @brief Handler for brewing
 *
 */
// TODO:
//  show sections on website only if needed
//  add pressure to shot timer?
//  backflush also as bool, enable from website over diffrent var
//  SteamOn also as bool, rethink enable from website

#pragma once

#include "brewStates.h"
#include "hardware/pumpControl.h"
#include "scaleHandler.h"
#include "utils/pressureProfile.h"
#include "utils/pressureProfileStorage.h"

// Brew control states
inline BrewSwitchState currBrewSwitchState = kBrewSwitchIdle;
inline BrewState currBrewState = kBrewIdle;
inline ManualFlushState currManualFlushState = kManualFlushIdle;
inline BackflushState currBackflushState = kBackflushIdle;

inline uint8_t brewSwitchReading = LOW;
inline uint8_t currReadingBrewSwitch = LOW;
inline bool brewSwitchWasOff = false;

// Brew values
inline double targetBrewTime = TARGET_BREW_TIME;           // brew time in s
inline double preinfusion = PRE_INFUSION_TIME;             // preinfusion time in s
inline double preinfusionPause = PRE_INFUSION_PAUSE_TIME;  // preinfusion pause time in s
inline double totalTargetBrewTime = 0;                     // total target brew time including preinfusion and preinfusion pause
inline double preinfusionPressure = PRE_INFUSION_PRESSURE; // pressure target during pre-infusion in bar (pressure profiling)
inline double brewPressure = BREW_PRESSURE;                // pressure target during main extraction in bar (pressure profiling)
inline double currBrewTime = 0;                            // current running total brewed time
inline unsigned long startingTime = 0;                     // start time of brew
inline bool brewPidDisabled = false;                       // is PID disabled for delay after brew has started?
inline bool brewUsesProfile = false;                       // current brew follows a multi-phase pressure profile
inline PressureProfileRunner pressureProfileRunner;        // steps through the profile of the current brew

// Backflush values
inline int backflushCycles = BACKFLUSH_CYCLES;
inline double backflushFillTime = BACKFLUSH_FILL_TIME;
inline double backflushFlushTime = BACKFLUSH_FLUSH_TIME;
inline bool backflushOn = false;
inline int currBackflushCycles = 1;

bool isPowerSwitchOperationAllowed();

/**
 * @brief True if in an intermediate brew state, false if idle or finished
 */
inline bool checkBrewActive() {
    return (currBrewState != kBrewIdle && currBrewState != kBrewFinished); // removed && !(machineState >= kEmergencyStop)
}

/**
 * @brief True if the brew should be pressure profiled (dimmer modulates pump power towards pressure targets)
 */
inline bool pressureProfilingEnabled() {
    return !pumpProfilingFaulted && config.get<bool>("brew.profiling.enabled") && config.get<bool>("hardware.pump.dimmer.enabled") && config.get<bool>("hardware.sensors.pressure.enabled");
}

/**
 * @brief The profile the next brew should follow, nullptr for a brew without profile (simple mode, plain relay)
 */
inline const PressureProfile* profileForNextBrew() {
    const PressureProfile* profile = activePressureProfile();

    if (profile == nullptr || !config.get<bool>("brew.profiling.enabled") || !config.get<bool>("hardware.pump.dimmer.enabled")) {
        return nullptr;
    }

    // fixed power phases work without a pressure sensor, pressure phases and pressure exit targets do not
    if (profile->needsPressureSensor() && !config.get<bool>("hardware.sensors.pressure.enabled")) {
        LOGF(WARNING, "Profile '%s' needs the pressure sensor, brewing without profile", profile->name);
        return nullptr;
    }

    return profile;
}

/**
 * @brief True if in a machine state related to brew or flush, false if in other states
 */
inline bool checkBrewStates() {
    return (machineState == kBrew || machineState == kBackflush || machineState == kManualFlush);
}

/**
 * @brief turns off valve if not in an active brew state or if machineState changes away from one related to brewing or flushing
 */
inline void valveSafetyShutdownCheck() {
    if (!checkBrewActive() && !checkBrewStates()) {
        valveRelay->off();
    }
}

/**
 * @brief Toggle or momentary input for Brew Switch
 */
inline void checkBrewSwitch() {
    if (!isPowerSwitchOperationAllowed()) {
        return;
    }

    static bool loggedEmptyWaterTank = false;
    brewSwitchReading = brewSwitch->isPressed();

    // Block brewSwitch input when water tank is empty
    if (machineState == kWaterTankEmpty) {

        if (!loggedEmptyWaterTank && (currBrewSwitchState == kBrewSwitchIdle || currBrewSwitchState == kBrewSwitchPressed)) {
            LOG(WARNING, "Brew switch input ignored: Water tank empty");
            loggedEmptyWaterTank = true;
        }
        return;
    }

    // Block brewSwitch input while hot water is being drawn
    if (machineState == kHotWater) {
        return;
    }

    loggedEmptyWaterTank = false;

    // Convert toggle brew switch input to brew switch state
    if (const int brewSwitchType = config.get<int>("hardware.switches.brew.type"); brewSwitchType == Switch::TOGGLE) {
        if (currReadingBrewSwitch != brewSwitchReading) {
            currReadingBrewSwitch = brewSwitchReading;
        }

        switch (currBrewSwitchState) {
            case kBrewSwitchIdle:
                if (currReadingBrewSwitch == HIGH) {
                    currBrewSwitchState = kBrewSwitchShortPressed;
                    LOG(DEBUG, "Toggle Brew switch is ON -> got to currBrewSwitchState = kBrewSwitchShortPressed");
                }
                break;

            case kBrewSwitchShortPressed:
                if (currReadingBrewSwitch == LOW) {
                    currBrewSwitchState = kBrewSwitchIdle;
                    LOG(DEBUG, "Toggle Brew switch is OFF -> got to currBrewSwitchState = kBrewSwitchIdle");
                }
                else if (currBrewState == kBrewFinished || currBackflushState == kBackflushFinished) {
                    currBrewSwitchState = kBrewSwitchWaitForRelease;
                    LOG(DEBUG, "Brew reached target or backflush done -> got to currBrewSwitchState = kBrewSwitchWaitForRelease");
                }
                break;

            case kBrewSwitchWaitForRelease:
                if (currReadingBrewSwitch == LOW) {
                    currBrewSwitchState = kBrewSwitchIdle;
                    LOG(DEBUG, "Brew switch reset -> got to currBrewSwitchState = kBrewSwitchIdle");
                }
                break;

            default:

                currBrewSwitchState = kBrewSwitchIdle;
                LOG(DEBUG, "Unexpected switch state -> currBrewSwitchState = kBrewSwitchIdle");
                break;
        }
    }
    // Convert momentary brew switch input to brew switch state
    else if (brewSwitchType == Switch::MOMENTARY) {
        if (currReadingBrewSwitch != brewSwitchReading) {
            currReadingBrewSwitch = brewSwitchReading;
        }

        switch (currBrewSwitchState) {
            case kBrewSwitchIdle:
                if (currReadingBrewSwitch == HIGH) {
                    currBrewSwitchState = kBrewSwitchPressed;
                    LOG(DEBUG, "Brew switch press detected -> got to currBrewSwitchState = kBrewSwitchPressed");
                }
                break;

            case kBrewSwitchPressed:                // Brew switch pressed - check for short or long press
                if (currReadingBrewSwitch == LOW) { // Brew switch short press detected
                    currBrewSwitchState = kBrewSwitchShortPressed;
                    LOG(DEBUG, "Brew switch short press detected -> got to currBrewSwitchState = kBrewSwitchShortPressed; start brew");
                }
                else if (currReadingBrewSwitch == HIGH && brewSwitch->longPressDetected()) { // Brew switch long press detected
                    currBrewSwitchState = kBrewSwitchLongPressed;
                    LOG(DEBUG, "Brew switch long press detected -> got to currBrewSwitchState = kBrewSwitchLongPressed; start manual flush");
                }
                break;

            case kBrewSwitchShortPressed:
                if (currReadingBrewSwitch == HIGH) { // Brew switch short press detected while brew is running - abort brew
                    currBrewSwitchState = kBrewSwitchWaitForRelease;
                    LOG(DEBUG, "Brew switch short press detected -> got to currBrewSwitchState = kBrewSwitchWaitForRelease; brew or backflush stopped manually");
                }
                else if (currBrewState == kBrewFinished || currBackflushState == kBackflushFinished) { // Brew reached target and stopped or blackflush cycle done
                    currBrewSwitchState = kBrewSwitchWaitForRelease;
                    LOG(DEBUG, "Brew reached target or backflush done -> got to currBrewSwitchState = kBrewSwitchWaitForRelease");
                }
                break;

            case kBrewSwitchLongPressed:
                if (currReadingBrewSwitch == LOW) { // Brew switch got released after long press detected - reset brewswitch
                    currBrewSwitchState = kBrewSwitchWaitForRelease;
                    LOG(DEBUG, "Brew switch long press released -> got to currBrewSwitchState = kBrewSwitchWaitForRelease; stop manual flush");
                }
                break;

            case kBrewSwitchWaitForRelease: // wait for brew switch got released
                if (currReadingBrewSwitch == LOW) {
                    currBrewSwitchState = kBrewSwitchIdle;
                    LOG(DEBUG, "Brew switch reset -> got to currBrewSwitchState = kBrewSwitchIdle");
                }
                break;

            default:
                currBrewSwitchState = kBrewSwitchIdle;
                LOG(DEBUG, "Unexpected switch state -> currBrewSwitchState = kBrewSwitchIdle");
                break;
        }
    }
}

/**
 * @brief If set to publish debug messages then list what the current action is and what triggered it
 * @return void
 */
inline void debugPumpState(String label, String state) {
    hotWaterStateDebug = state;
    if (hotWaterStateDebug != lastHotWaterStateDebug) {
        LOGF(DEBUG, "Hot water state: %s - BrewHandler: %s", hotWaterStateDebug, label);
        lastHotWaterStateDebug = hotWaterStateDebug;
    }
}

/**
 * @brief Brew process handeling including timer and state machine for brew-by-time and brew-by-weight
 * @return true if brew is running, false otherwise
 */
inline bool brew() {
    if (!config.get<bool>("hardware.switches.brew.enabled") || brewSwitch == nullptr) {
        return false; // brew switch is not enabled, so no brew process running
    }

    const unsigned long currentMillisTemp = millis();
    checkBrewSwitch();

    // abort function for state machine from every state
    if (currBrewSwitchState == kBrewSwitchIdle && currBrewState > kBrewIdle && currBrewState < kBrewFinished) {
        if (currBrewState != kBrewFinished) {
            LOG(INFO, "Brew stopped manually");
        }
        currBrewState = kBrewFinished;
    }
    // calculated brew time while brew is running
    if (currBrewState > kBrewIdle && currBrewState < kBrewFinished) {
        currBrewTime = currentMillisTemp - startingTime;
    }

    const int brewMode = config.get<int>("brew.mode");
    const bool brewByTimeEnabled = brewMode != 0 && config.get<bool>("brew.by_time.enabled");
    const bool brewByWeightEnabled = brewMode != 0 && config.get<bool>("brew.by_weight.enabled");
    const bool preinfusionEnabled = config.get<bool>("brew.pre_infusion.enabled");

    // check if brewswitch was turned off after a brew; Brew only runs once even brewswitch is still pressed
    if (currBrewSwitchState == kBrewSwitchIdle) {
        brewSwitchWasOff = true;
    }

    // set brew time every cycle, in case changes are done during brew
    if (targetBrewTime > 0) {
        totalTargetBrewTime = targetBrewTime * 1000;

        if (preinfusionEnabled) {
            totalTargetBrewTime += preinfusion * 1000 + preinfusionPause * 1000;
        }
    }
    else {
        // Stop by time deactivated --> totalTargetBrewTime = 0
        totalTargetBrewTime = 0;
    }

    // a profiled brew ends with its profile, show the nominal profile length as target time
    if (brewUsesProfile) {
        totalTargetBrewTime = pressureProfileRunner.profile().totalDuration() * 1000;
    }

    // state machine for brew
    switch (currBrewState) {
        case kBrewIdle:                       // waiting step for brew switch turning on
            if (currBrewSwitchState == kBrewSwitchShortPressed && brewSwitchWasOff && !backflushOn && machineState != kBackflush) {
                startingTime = millis();
                currBrewTime = 0;             // reset currBrewTime, last brew is still stored
                currBrewWeight = 0;           // reset currBrewWeight for new brew
                pumpProfilingFaulted = false; // give pressure profiling a new chance after a sensor fault in the last brew

                LOG(INFO, "Brew started");

                // a selected profile replaces pre-infusion and the time target, its phases define the whole shot
                const PressureProfile* profile = profileForNextBrew();
                brewUsesProfile = profile != nullptr;

                if (brewUsesProfile) {
                    pressureProfileRunner.start(*profile, 0, inputPressureFilter);
                    LOGF(INFO, "Brew running with pressure profile '%s' (%u phases, %.0f s)", profile->name, profile->phaseCount, profile->totalDuration());
                    currBrewState = kBrewRunning;
                }
                else if (!preinfusionEnabled) {
                    LOG(INFO, "Brew running");
                    currBrewState = kBrewRunning;
                }
                else if (preinfusion == 0) {
                    LOG(INFO, "Preinfusion was zero, Preinfusion pause running");
                    currBrewState = kPreinfusionPause;
                }
                else {
                    LOG(INFO, "Preinfusion running");
                    currBrewState = kPreinfusion;
                }

                if (scale && config.get<bool>("hardware.sensors.scale.enabled") && config.get<int>("hardware.sensors.scale.type") == 2) {
                    const auto bleScale = static_cast<BluetoothScale*>(scale);

                    if (config.get<bool>("display.blescale_brew_timer")) {
                        bleScale->resetTimer();
                        bleScale->startTimer();
                    }

                    if (config.get<bool>("brew.by_weight.enabled") && config.get<bool>("brew.by_weight.auto_tare")) {
                        // only send tare command if not already close to zero
                        if (abs(currReadingWeight) > 0.2) {
                            LOG(INFO, "Tare scale");
                            bleScale->tare();
                            // Mark that auto-tare is in progress for Bluetooth scales
                            autoTareInProgress = true;
                            autoTareStartTime = millis();
                        }
                    }
                }
            }

            break;

        case kPreinfusion:
            valveRelay->on();

            if (pressureProfilingEnabled()) {
                if (!pumpPsmActive) {
                    startPumpProfiling(static_cast<float>(preinfusionPressure));
                }

                pumpPressureTarget = static_cast<float>(preinfusionPressure);
                debugPumpState("Preinfusion", "profiling");
            }
            else {
                pumpRelay->on();
                debugPumpState("Preinfusion", "on");
            }

            if (currBrewTime > preinfusion * 1000) {
                LOG(INFO, "Preinfusion pause running");
                currBrewState = kPreinfusionPause;
            }

            break;

        case kPreinfusionPause:
            valveRelay->on();
            stopPumpProfiling();
            pumpRelay->off();
            debugPumpState("Pause", "off");

            if (currBrewTime > (preinfusion + preinfusionPause) * 1000) {
                LOG(INFO, "Brew running");
                currBrewState = kBrewRunning;
            }

            break;

        case kBrewRunning:
            {
                valveRelay->on();

                // the profile keeps running after a sensor fault (full pump power then), it still decides when the shot ends
                const PumpSetpoint setpoint =
                    brewUsesProfile ? pressureProfileRunner.update(static_cast<uint32_t>(currBrewTime), inputPressureFilter, currBrewWeight, pumpPsmDuty) : PumpSetpoint{PumpMode::Pressure, static_cast<float>(brewPressure)};

                if (brewUsesProfile && pressureProfileRunner.finished()) {
                    LOG(INFO, "Brew reached end of profile");
                    currBrewState = kBrewFinished;
                }
                else if (brewUsesProfile ? !pumpProfilingFaulted : pressureProfilingEnabled()) {
                    if (!pumpPsmActive) {
                        startPumpProfiling(setpoint.mode == PumpMode::Pressure ? setpoint.value : 0.0f);
                    }

                    if (setpoint.mode == PumpMode::Power) {
                        setPumpPower(setpoint.value);
                    }
                    else {
                        setPumpPressureTarget(setpoint.value);
                    }

                    debugPumpState("BrewRunning", "profiling");
                }
                else if (brewUsesProfile && setpoint.value < pumpPressureOffThreshold) {
                    // pump off phase (bloom / pause) of a profile that lost its pressure sensor
                    pumpRelay->off();
                    debugPumpState("BrewRunning", "off");
                }
                else {
                    pumpRelay->on();
                    debugPumpState("BrewRunning", "on");
                }

                if (currBrewState == kBrewFinished) {
                    // profile finished above
                }
                else if (currBrewTime > totalTargetBrewTime && brewByTimeEnabled && !brewUsesProfile) {
                    LOG(INFO, "Brew reached time target");
                    currBrewState = kBrewFinished;
                }
                else if (scale && config.get<bool>("hardware.sensors.scale.enabled")) {
                    const auto targetBrewWeight = ParameterRegistry::getInstance().getParameterById("brew.by_weight.target_weight")->getValueAs<float>();

                    if (currBrewWeight > targetBrewWeight && brewByWeightEnabled) {
                        LOG(INFO, "Brew reached weight target");
                        currBrewState = kBrewFinished;
                    }
                }

                break;
            }

        case kBrewFinished:
            {
                valveRelay->off();
                stopPumpProfiling();
                pumpRelay->off();
                debugPumpState("BrewFinished", "off");

                pressureProfileRunner.stop();
                brewUsesProfile = false;

                brewSwitchWasOff = false;
                LOG(INFO, "Brew finished");
                LOGF(INFO, "Shot time: %4.1f s", currBrewTime / 1000);
                LOG(INFO, "Brew idle");
                currBrewState = kBrewIdle;

                if (scale && config.get<bool>("hardware.sensors.scale.enabled") && config.get<int>("hardware.sensors.scale.type") == 2 && config.get<bool>("display.blescale_brew_timer")) {
                    static_cast<BluetoothScale*>(scale)->stopTimer();
                }

                break;
            }

        default:
            currBrewState = kBrewIdle;
            LOG(DEBUG, "Unexpected brew state -> currBrewState = kBrewIdle");

            break;
    }

    return checkBrewActive();
}

/**
 * @brief manual grouphead flush
 * @return true if manual flush is running, false otherwise
 */
inline bool manualFlush() {
    if (!config.get<bool>("hardware.switches.brew.enabled") || brewSwitch == nullptr) {
        return false; // brew switch is not enabled, so no brew process running
    }

    const unsigned long currentMillisTemp = millis();
    checkBrewSwitch();

    if (currManualFlushState == kManualFlushRunning) {
        currBrewTime = currentMillisTemp - startingTime;
    }

    switch (currManualFlushState) {
        case kManualFlushIdle:
            if (currBrewSwitchState == kBrewSwitchLongPressed) {
                startingTime = millis();
                valveRelay->on();
                pumpRelay->on();
                debugPumpState("ManualFlush", "on");
                LOG(INFO, "Manual flush started");
                currManualFlushState = kManualFlushRunning;
            }
            break;

        case kManualFlushRunning:
            if (currBrewSwitchState != kBrewSwitchLongPressed) {
                valveRelay->off();
                pumpRelay->off();
                debugPumpState("ManualFlush", "off");
                LOG(INFO, "Manual flush stopped");
                LOGF(INFO, "Manual flush time: %4.1f s", currBrewTime / 1000);
                currManualFlushState = kManualFlushIdle;
            }
            break;

        default:
            currManualFlushState = kManualFlushIdle;
            LOG(DEBUG, "Unexpected manual flush state -> currManualFlushState = kManualFlushIdle");

            break;
    }

    return currManualFlushState == kManualFlushRunning;
}

/**
 * @brief Backflush
 */
inline void backflush() {
    if (!config.get<bool>("hardware.switches.brew.enabled") || brewSwitch == nullptr) {
        return; // brew switch is not enabled, so no brew process running
    }

    checkBrewSwitch();

    if (currBackflushState != kBackflushIdle && !backflushOn) {
        currBackflushState = kBackflushFinished; // Force reset in case backflushOn is reset during backflush!
        LOG(INFO, "Backflush: Disabled via webinterface");
    }
    else if (offlineMode || currBrewState > kBrewIdle || backflushCycles <= 0 || !backflushOn) {
        return;
    }

    // abort function for state machine from every state
    if (currBrewSwitchState == kBrewSwitchIdle && currBackflushState > kBackflushIdle && currBackflushState < kBackflushFinished) {
        currBackflushState = kBackflushFinished;

        LOG(INFO, "Backflush stopped manually");
    }

    // check if brewswitch was turned off after a backflush; Backflush only runs once even brewswitch is still pressed
    if (currBrewSwitchState == kBrewSwitchIdle) {
        brewSwitchWasOff = true;
    }

    // State machine for backflush
    switch (currBackflushState) {
        case kBackflushIdle:
            if (currBrewSwitchState == kBrewSwitchShortPressed && backflushOn && brewSwitchWasOff) {
                startingTime = millis();
                valveRelay->on();
                pumpRelay->on();
                debugPumpState("Backflush", "on");
                LOGF(INFO, "Start backflush cycle %d", currBackflushCycles);
                LOG(INFO, "Backflush: filling portafilter");
                currBackflushState = kBackflushFilling;
            }

            break;

        case kBackflushFilling:
            if (millis() - startingTime > backflushFillTime * 1000) {
                startingTime = millis();
                valveRelay->off();
                pumpRelay->off();
                debugPumpState("Backflush", "off");
                LOG(INFO, "Backflush: flushing into drip tray");

                if (currBackflushCycles == backflushCycles) {
                    currBackflushState = kBackflushEnding;
                }
                else {
                    currBackflushState = kBackflushFlushing;
                }
            }

            break;

        case kBackflushFlushing:
            if (millis() - startingTime > backflushFlushTime * 1000) {
                if (currBackflushCycles < backflushCycles) {
                    startingTime = millis();
                    valveRelay->on();
                    pumpRelay->on();
                    debugPumpState("Backflush", "on");
                    currBackflushCycles++;
                    LOGF(INFO, "Backflush: next backflush cycle %d", currBackflushCycles);
                    LOG(INFO, "Backflush: filling portafilter");
                    currBackflushState = kBackflushFilling;
                }
                else {
                    currBackflushState = kBackflushFinished;
                }
            }

            break;

        case kBackflushEnding:
            if (millis() - startingTime > backflushFlushTime * 1000) {
                currBackflushState = kBackflushFinished;
            }

            break;

        case kBackflushFinished:
            valveRelay->off();
            pumpRelay->off();
            debugPumpState("Backflush", "off");
            LOGF(INFO, "Backflush finished after %d cycles", currBackflushCycles);
            currBackflushCycles = 1;
            brewSwitchWasOff = false;
            currBackflushState = kBackflushIdle;

            break;

        default:
            currBackflushState = kBackflushIdle;
            LOG(DEBUG, "Unexpected backflush state -> currBackflushState = kBackflushIdle");

            break;
    }
}
