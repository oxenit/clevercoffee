/**
 * @file pressureProfile.h
 *
 * @brief Multi-phase brew profiles and the runner that turns a profile into a pump setpoint over the course of a brew.
 *
 *        The data model follows the GaggiMate profile format (https://github.com/jniebuhr/gaggimate, schema/profile.json),
 *        restricted to what this firmware can do: a phase either regulates towards a pressure target (needs the pressure
 *        sensor) or runs the pump at a fixed power. Flow targets are not supported (no flow model).
 *
 *        A phase reaches its setpoint through a transition (instant step, or a linear/eased ramp over a time in seconds)
 *        and ends after its duration, or earlier when one of its exit targets (cup weight, measured pressure, compared with
 *        >= or <=) is reached. A pressure phase with 0 bar switches the pump off with the brew valve open (bloom / pause).
 *
 *        Ramps start from the setpoint the previous phase was producing at the moment of the switch, if it was in the same
 *        mode. A pressure phase following a power phase ramps from the measured pressure, a power phase following a
 *        pressure phase starts from the pump power at the switch.
 *
 *        This file is deliberately free of Arduino dependencies so the runner can be compiled and tested on the host.
 */

#pragma once

#include <cstddef>
#include <cstdint>

constexpr uint8_t kMaxPressureProfiles = 8;
constexpr uint8_t kMaxProfilePhases = 8;
constexpr uint8_t kMaxPhaseTargets = 4;
constexpr size_t kProfileNameLength = 24;
constexpr float kProfileMaxPressure = 12.0f;   // bar
constexpr float kProfileMaxPhaseTime = 300.0f; // s, same per phase safety limit as GaggiMate
constexpr float kProfileMaxWeight = 200.0f;    // g

enum class ProfileTransition : uint8_t {
    Instant = 0,
    Linear,
    EaseIn,
    EaseOut,
    EaseInOut,
};

enum class PumpMode : uint8_t {
    Pressure = 0, // regulate towards a pressure [bar]
    Power,        // fixed pump power [%], no pressure feedback
};

enum class PhaseTargetType : uint8_t {
    Weight = 0, // cup weight [g] ("volumetric" in GaggiMate)
    Pressure,   // measured pressure [bar]
};

struct PhaseTarget {
        PhaseTargetType type = PhaseTargetType::Weight;
        bool lessOrEqual = false; // false: value >= target ("gte"), true: value <= target ("lte")
        float value = 0.0f;
};

struct ProfilePhase {
        char name[kProfileNameLength] = "";
        PumpMode mode = PumpMode::Pressure;
        float setpoint = 0.0f;           // pressure [bar] or pump power [%], depending on mode
        float duration = 0.0f;           // maximum phase length [s], includes the transition
        ProfileTransition transition = ProfileTransition::Instant;
        float transitionDuration = 0.0f; // ramp length [s], clamped to the phase duration, 0 = whole phase
        uint8_t targetCount = 0;
        PhaseTarget targets[kMaxPhaseTargets];
};

struct PressureProfile {
        char name[kProfileNameLength] = "";
        uint8_t phaseCount = 0;
        ProfilePhase phases[kMaxProfilePhases];

        /**
         * @brief Nominal duration of the profile [s] (sum of the phase durations, ignoring early exits)
         */
        float totalDuration() const {
            float total = 0.0f;

            for (uint8_t i = 0; i < phaseCount; i++) {
                total += phases[i].duration;
            }

            return total;
        }

        bool needsPressureSensor() const {
            for (uint8_t i = 0; i < phaseCount; i++) {
                if (phases[i].mode == PumpMode::Pressure) {
                    return true;
                }

                for (uint8_t t = 0; t < phases[i].targetCount; t++) {
                    if (phases[i].targets[t].type == PhaseTargetType::Pressure) {
                        return true;
                    }
                }
            }

            return false;
        }
};

struct PumpSetpoint {
        PumpMode mode = PumpMode::Power;
        float value = 0.0f; // bar or %
};

/**
 * @brief Map the linear progress t (0..1) of a transition onto its curve
 */
inline float profileTransitionCurve(const ProfileTransition type, const float t) {
    if (t <= 0.0f) {
        return type == ProfileTransition::Instant ? 1.0f : 0.0f;
    }

    if (t >= 1.0f) {
        return 1.0f;
    }

    switch (type) {
        case ProfileTransition::Instant:
            return 1.0f;
        case ProfileTransition::Linear:
            return t;
        case ProfileTransition::EaseIn:
            return t * t;
        case ProfileTransition::EaseOut:
            return 1.0f - (1.0f - t) * (1.0f - t);
        case ProfileTransition::EaseInOut:
            return t < 0.5f ? 2.0f * t * t : 1.0f - 2.0f * (1.0f - t) * (1.0f - t);
    }

    return t;
}

/**
 * @brief Steps through a profile during a brew. Works on its own copy of the profile, so editing the
 *        stored profiles while a brew is running does not affect it.
 */
class PressureProfileRunner {
    public:
        /**
         * @param profile profile to run (copied)
         * @param nowMs brew time at the start [ms]
         * @param pressure measured pressure at the start [bar]
         */
        void start(const PressureProfile& profile, const uint32_t nowMs, const float pressure) {
            profile_ = profile;
            phaseIndex_ = 0;
            running_ = profile_.phaseCount > 0;
            output_ = PumpSetpoint();

            if (running_) {
                enterPhase(nowMs, pressure, 0.0f);
            }
        }

        void stop() {
            running_ = false;
        }

        /**
         * @brief Advance the profile and return the pump setpoint for this moment
         *
         * @param nowMs current brew time [ms]
         * @param pressure measured pressure [bar]
         * @param weight current cup weight [g], pass 0 without a scale
         * @param pumpPower current pump power [%], start value when switching from a pressure to a power phase
         * @return setpoint, power 0 once the profile is finished
         */
        PumpSetpoint update(const uint32_t nowMs, const float pressure, const float weight, const float pumpPower) {
            // advance through every phase that is already over (loop, so zero length phases are skipped in one call)
            while (running_ && phaseOver(nowMs, pressure, weight)) {
                // a phase that ran its full duration ends at its nominal end, so late updates do not shift the schedule
                const uint32_t durationEndMs = phaseStartMs_ + static_cast<uint32_t>(profile_.phases[phaseIndex_].duration * 1000.0f);
                const uint32_t nextStartMs = static_cast<int32_t>(nowMs - durationEndMs) >= 0 ? durationEndMs : nowMs;
                phaseIndex_++;

                if (phaseIndex_ >= profile_.phaseCount) {
                    running_ = false;
                    break;
                }

                enterPhase(nextStartMs, pressure, pumpPower);
            }

            if (!running_) {
                output_ = PumpSetpoint();
                return output_;
            }

            const ProfilePhase& phase = profile_.phases[phaseIndex_];
            const float elapsed = static_cast<float>(nowMs - phaseStartMs_) / 1000.0f;
            float rampDuration = phase.transitionDuration < phase.duration ? phase.transitionDuration : phase.duration;

            if (rampDuration <= 0.0f) {
                rampDuration = phase.duration;
            }

            output_.mode = phase.mode;

            if (phase.transition == ProfileTransition::Instant || rampDuration <= 0.0f) {
                output_.value = phase.setpoint;
            }
            else {
                const float progress = profileTransitionCurve(phase.transition, elapsed / rampDuration);
                output_.value = phaseStartValue_ + (phase.setpoint - phaseStartValue_) * progress;
            }

            return output_;
        }

        bool finished() const {
            return !running_;
        }

        uint8_t phaseIndex() const {
            return phaseIndex_;
        }

        const PressureProfile& profile() const {
            return profile_;
        }

    private:
        void enterPhase(const uint32_t nowMs, const float pressure, const float pumpPower) {
            const PumpMode mode = profile_.phases[phaseIndex_].mode;
            phaseStartMs_ = nowMs;

            if (mode == output_.mode && phaseIndex_ > 0) {
                phaseStartValue_ = output_.value; // continue from where the previous phase was
            }
            else {
                phaseStartValue_ = mode == PumpMode::Pressure ? pressure : pumpPower;
            }

            output_.mode = mode;
            output_.value = phaseStartValue_;
        }

        bool phaseOver(const uint32_t nowMs, const float pressure, const float weight) const {
            const ProfilePhase& phase = profile_.phases[phaseIndex_];
            const float elapsed = static_cast<float>(nowMs - phaseStartMs_) / 1000.0f;

            if (elapsed >= phase.duration) {
                return true;
            }

            for (uint8_t i = 0; i < phase.targetCount; i++) {
                const PhaseTarget& target = phase.targets[i];

                // a weight of 0 means "no target" (GaggiMate semantics), also avoids stopping at once without a scale
                if (target.type == PhaseTargetType::Weight && target.value <= 0.0f) {
                    continue;
                }

                const float value = target.type == PhaseTargetType::Weight ? weight : pressure;

                if (target.lessOrEqual ? value <= target.value : value >= target.value) {
                    return true;
                }
            }

            return false;
        }

        PressureProfile profile_;
        uint8_t phaseIndex_ = 0;
        bool running_ = false;
        uint32_t phaseStartMs_ = 0;
        float phaseStartValue_ = 0.0f;
        PumpSetpoint output_;
};
