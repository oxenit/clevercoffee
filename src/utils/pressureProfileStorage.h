/**
 * @file pressureProfileStorage.h
 *
 * @brief Persistence of the pressure profiles in /profiles.json on LittleFS.
 *
 *        The profiles live in their own file instead of config.json because they are a variable length list, which the
 *        ParameterRegistry (scalar settings only) cannot represent. The profile objects use the GaggiMate field names
 *        (see pressureProfile.h for the supported subset), so pressure based GaggiMate profiles can be imported:
 *
 *        {
 *          "selected": 0,                // index of the active profile, -1 = simple mode (pre-infusion/brew pressure settings)
 *          "profiles": [
 *            {
 *              "label": "Blooming",
 *              "phases": [
 *                { "name": "Fill", "duration": 10, "pump": { "target": "pressure", "pressure": 3 },
 *                  "transition": { "type": "instant", "duration": 0 },
 *                  "targets": [ { "type": "pressure", "operator": "gte", "value": 2.5 } ] },
 *                { "name": "Bloom", "duration": 10, "pump": 0 },
 *                ...
 *              ]
 *            }
 *          ]
 *        }
 *
 *        "pump" is either a fixed pump power in % or a pressure target. Exit targets: "volumetric" (cup weight, g) and
 *        "pressure" (bar), operator "gte" or "lte". A missing or invalid file is replaced by the default profiles.
 *
 *        Updates from the web server arrive on the AsyncTCP task. They are validated there, but only applied from the main
 *        loop (processPendingPressureProfiles) and never during a brew, so the brew handler never sees a half-written list.
 */

#pragma once

#include "Logger.h"
#include "pressureProfile.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <mutex>
#include <new>

constexpr const char* kPressureProfilesPath = "/profiles.json";
// compact JSON of 8 profiles x 8 phases x 4 exit targets with maximal names and values is about 25 kB
constexpr size_t kPressureProfilesMaxJsonSize = 32768;

inline PressureProfile pressureProfiles[kMaxPressureProfiles];
inline uint8_t pressureProfileCount = 0;
inline int8_t selectedPressureProfile = -1;

// validated update waiting for the main loop, owned by whoever holds pendingPressureProfilesMutex
struct PendingPressureProfiles {
        PressureProfile profiles[kMaxPressureProfiles];
        uint8_t count = 0;
        int8_t selected = -1;
};

inline std::mutex pendingPressureProfilesMutex;
inline PendingPressureProfiles* pendingPressureProfiles = nullptr;

inline const char* profileTransitionToString(const ProfileTransition type) {
    switch (type) {
        case ProfileTransition::Linear:
            return "linear";
        case ProfileTransition::EaseIn:
            return "ease-in";
        case ProfileTransition::EaseOut:
            return "ease-out";
        case ProfileTransition::EaseInOut:
            return "ease-in-out";
        default:
            return "instant";
    }
}

inline bool profileTransitionFromString(const char* value, ProfileTransition& type) {
    if (value == nullptr || strcmp(value, "instant") == 0) {
        type = ProfileTransition::Instant;
    }
    else if (strcmp(value, "linear") == 0) {
        type = ProfileTransition::Linear;
    }
    else if (strcmp(value, "ease-in") == 0) {
        type = ProfileTransition::EaseIn;
    }
    else if (strcmp(value, "ease-out") == 0) {
        type = ProfileTransition::EaseOut;
    }
    else if (strcmp(value, "ease-in-out") == 0) {
        type = ProfileTransition::EaseInOut;
    }
    else {
        return false;
    }

    return true;
}

/**
 * @brief Read a number from a JSON object and check its range, missing keys take the fallback value
 */
inline bool readProfileNumber(JsonObjectConst obj, const char* key, const float minValue, const float maxValue, const float fallback, float& out, String& error) {
    const JsonVariantConst value = obj[key];

    if (value.isNull()) {
        out = fallback;
        return true;
    }

    if (!value.is<float>()) {
        error = String("'") + key + "' must be a number";
        return false;
    }

    out = value.as<float>();

    if (out < minValue || out > maxValue) {
        error = String("'") + key + "' must be between " + String(minValue, 1) + " and " + String(maxValue, 1);
        return false;
    }

    return true;
}

/**
 * @brief Parse one phase, GaggiMate field names. Fields this firmware does not use (phase, valve, temperature,
 *        transition.adaptive, ...) are ignored, features it cannot do (flow targets, volume based ramps) are rejected.
 */
inline bool parseProfilePhase(JsonObjectConst json, ProfilePhase& phase, String& error) {
    phase = ProfilePhase();
    strlcpy(phase.name, json["name"] | "", sizeof(phase.name));

    if (!readProfileNumber(json, "duration", 0.0f, kProfileMaxPhaseTime, NAN, phase.duration, error)) {
        return false;
    }

    if (isnan(phase.duration)) {
        error = "'duration' is required";
        return false;
    }

    // pump: a number is a fixed power, an object a pressure (or flow) target
    const JsonVariantConst pump = json["pump"];

    if (pump.is<float>()) {
        phase.mode = PumpMode::Power;
        phase.setpoint = pump.as<float>();

        if (phase.setpoint < 0.0f || phase.setpoint > 100.0f) {
            error = "pump power must be between 0 and 100";
            return false;
        }
    }
    else if (pump.is<JsonObjectConst>()) {
        const char* target = pump["target"] | "pressure";

        if (strcmp(target, "pressure") != 0) {
            error = String("pump target '") + target + "' is not supported, only 'pressure'";
            return false;
        }

        phase.mode = PumpMode::Pressure;

        if (!readProfileNumber(pump, "pressure", 0.0f, kProfileMaxPressure, NAN, phase.setpoint, error)) {
            return false;
        }

        if (isnan(phase.setpoint)) {
            error = "'pump.pressure' is required";
            return false;
        }
    }
    else {
        error = "'pump' must be a power (0-100) or an object {\"target\": \"pressure\", \"pressure\": bar}";
        return false;
    }

    const JsonObjectConst transition = json["transition"];

    if (!transition.isNull()) {
        if (!profileTransitionFromString(transition["type"] | "instant", phase.transition)) {
            error = "unknown transition type";
            return false;
        }

        const char* transitionTarget = transition["target"] | "time";

        if (strcmp(transitionTarget, "time") != 0) {
            error = String("transition target '") + transitionTarget + "' is not supported, only 'time'";
            return false;
        }

        if (!readProfileNumber(transition, "duration", 0.0f, kProfileMaxPhaseTime, 0.0f, phase.transitionDuration, error)) {
            return false;
        }
    }

    const JsonArrayConst targets = json["targets"].as<JsonArrayConst>();

    if (targets.size() > kMaxPhaseTargets) {
        error = String("at most ") + kMaxPhaseTargets + " exit targets per phase";
        return false;
    }

    for (JsonObjectConst targetJson : targets) {
        PhaseTarget& target = phase.targets[phase.targetCount];
        const char* type = targetJson["type"] | "";
        float maxValue;

        if (strcmp(type, "volumetric") == 0) {
            target.type = PhaseTargetType::Weight;
            maxValue = kProfileMaxWeight;
        }
        else if (strcmp(type, "pressure") == 0) {
            target.type = PhaseTargetType::Pressure;
            maxValue = kProfileMaxPressure;
        }
        else {
            error = String("exit target type '") + type + "' is not supported, only 'volumetric' and 'pressure'";
            return false;
        }

        const char* op = targetJson["operator"] | "gte";

        if (strcmp(op, "gte") != 0 && strcmp(op, "lte") != 0) {
            error = "target operator must be 'gte' or 'lte'";
            return false;
        }

        target.lessOrEqual = strcmp(op, "lte") == 0;

        if (!readProfileNumber(targetJson, "value", 0.0f, maxValue, NAN, target.value, error)) {
            return false;
        }

        if (isnan(target.value)) {
            error = "target 'value' is required";
            return false;
        }

        phase.targetCount++;
    }

    return true;
}

/**
 * @brief Parse and validate a profiles document
 *
 * @param doc parsed JSON
 * @param profiles output array, kMaxPressureProfiles entries
 * @param count number of parsed profiles
 * @param selected parsed selection
 * @param error human readable reason when the document is rejected
 * @return true if the document is valid
 */
inline bool parsePressureProfiles(const JsonDocument& doc, PressureProfile* profiles, uint8_t& count, int8_t& selected, String& error) {
    const JsonArrayConst list = doc["profiles"].as<JsonArrayConst>();

    if (list.isNull()) {
        error = "'profiles' must be an array";
        return false;
    }

    if (list.size() > kMaxPressureProfiles) {
        error = String("At most ") + kMaxPressureProfiles + " profiles are supported";
        return false;
    }

    count = 0;

    for (JsonObjectConst profileJson : list) {
        PressureProfile& profile = profiles[count];
        profile = PressureProfile();

        const char* name = profileJson["label"] | "";

        if (strlen(name) == 0) {
            error = String("Profile ") + (count + 1) + " has no label";
            return false;
        }

        strlcpy(profile.name, name, sizeof(profile.name));

        const JsonArrayConst phases = profileJson["phases"].as<JsonArrayConst>();

        if (phases.isNull() || phases.size() == 0 || phases.size() > kMaxProfilePhases) {
            error = String("Profile '") + profile.name + "' needs 1 to " + kMaxProfilePhases + " phases";
            return false;
        }

        for (JsonObjectConst phaseJson : phases) {
            String phaseError;

            if (!parseProfilePhase(phaseJson, profile.phases[profile.phaseCount], phaseError)) {
                error = String("Profile '") + profile.name + "', phase " + (profile.phaseCount + 1) + ": " + phaseError;
                return false;
            }

            profile.phaseCount++;
        }

        count++;
    }

    const int selectedIndex = doc["selected"] | -1;

    if (selectedIndex < -1 || selectedIndex >= count) {
        error = "'selected' must be -1 (simple mode) or the index of an existing profile";
        return false;
    }

    selected = static_cast<int8_t>(selectedIndex);

    return true;
}

inline void serializePressureProfiles(JsonDocument& doc) {
    doc["selected"] = selectedPressureProfile;
    const JsonArray list = doc["profiles"].to<JsonArray>();

    for (uint8_t i = 0; i < pressureProfileCount; i++) {
        const PressureProfile& profile = pressureProfiles[i];
        const JsonObject profileJson = list.add<JsonObject>();
        profileJson["label"] = profile.name;
        profileJson["type"] = "pro";
        const JsonArray phases = profileJson["phases"].to<JsonArray>();

        for (uint8_t p = 0; p < profile.phaseCount; p++) {
            const ProfilePhase& phase = profile.phases[p];
            const JsonObject phaseJson = phases.add<JsonObject>();
            phaseJson["name"] = phase.name;
            phaseJson["duration"] = phase.duration;

            if (phase.mode == PumpMode::Power) {
                phaseJson["pump"] = phase.setpoint;
            }
            else {
                const JsonObject pump = phaseJson["pump"].to<JsonObject>();
                pump["target"] = "pressure";
                pump["pressure"] = phase.setpoint;
            }

            const JsonObject transition = phaseJson["transition"].to<JsonObject>();
            transition["type"] = profileTransitionToString(phase.transition);
            transition["duration"] = phase.transitionDuration;

            const JsonArray targets = phaseJson["targets"].to<JsonArray>();

            for (uint8_t t = 0; t < phase.targetCount; t++) {
                const JsonObject target = targets.add<JsonObject>();
                target["type"] = phase.targets[t].type == PhaseTargetType::Weight ? "volumetric" : "pressure";
                target["operator"] = phase.targets[t].lessOrEqual ? "lte" : "gte";
                target["value"] = phase.targets[t].value;
            }
        }
    }
}

inline ProfilePhase& addDefaultPhase(PressureProfile& profile, const char* name, const float pressure, const float duration, const ProfileTransition transition, const float transitionDuration) {
    ProfilePhase& phase = profile.phases[profile.phaseCount++];
    strlcpy(phase.name, name, sizeof(phase.name));
    phase.mode = PumpMode::Pressure;
    phase.setpoint = pressure;
    phase.duration = duration;
    phase.transition = transition;
    phase.transitionDuration = transitionDuration;
    return phase;
}

/**
 * @brief A few starting points; simple mode stays selected so existing machines behave as before
 */
inline void createDefaultPressureProfiles() {
    pressureProfileCount = 0;

    PressureProfile& classic = pressureProfiles[pressureProfileCount++];
    classic = PressureProfile();
    strlcpy(classic.name, "Classic 9 bar", sizeof(classic.name));
    addDefaultPhase(classic, "Pre-infusion", 3.0f, 8.0f, ProfileTransition::Linear, 2.0f);
    addDefaultPhase(classic, "Extraction", 9.0f, 27.0f, ProfileTransition::Linear, 3.0f);

    PressureProfile& blooming = pressureProfiles[pressureProfileCount++];
    blooming = PressureProfile();
    strlcpy(blooming.name, "Blooming", sizeof(blooming.name));
    ProfilePhase& fill = addDefaultPhase(blooming, "Fill", 3.0f, 10.0f, ProfileTransition::Instant, 0.0f);
    fill.targets[0] = {PhaseTargetType::Pressure, false, 2.5f}; // puck saturated once pressure builds up
    fill.targetCount = 1;
    addDefaultPhase(blooming, "Bloom", 0.0f, 10.0f, ProfileTransition::Instant, 0.0f);
    addDefaultPhase(blooming, "Extraction", 9.0f, 25.0f, ProfileTransition::Linear, 4.0f);

    PressureProfile& lever = pressureProfiles[pressureProfileCount++];
    lever = PressureProfile();
    strlcpy(lever.name, "Lever", sizeof(lever.name));
    addDefaultPhase(lever, "Pre-infusion", 3.0f, 8.0f, ProfileTransition::Linear, 2.0f);
    addDefaultPhase(lever, "Peak", 9.0f, 8.0f, ProfileTransition::Linear, 4.0f);
    addDefaultPhase(lever, "Decline", 5.0f, 20.0f, ProfileTransition::Linear, 20.0f);

    selectedPressureProfile = -1;
}

inline bool savePressureProfiles() {
    JsonDocument doc;
    serializePressureProfiles(doc);

    File file = LittleFS.open(kPressureProfilesPath, "w");

    if (!file) {
        LOG(ERROR, "Failed to open pressure profiles file for writing");
        return false;
    }

    const size_t written = serializeJson(doc, file);
    file.close();

    if (written == 0) {
        LOG(ERROR, "Failed to write pressure profiles file");
        return false;
    }

    return true;
}

/**
 * @brief Load the profiles at boot, (re)creating the defaults when the file is missing or invalid
 */
inline void loadPressureProfiles() {
    if (LittleFS.exists(kPressureProfilesPath)) {
        File file = LittleFS.open(kPressureProfilesPath, "r");
        JsonDocument doc;
        const DeserializationError jsonError = file ? deserializeJson(doc, file) : DeserializationError::IncompleteInput;

        if (file) {
            file.close();
        }

        String error = jsonError ? String(jsonError.c_str()) : String();
        uint8_t count = 0;
        int8_t selected = -1;

        // parse into a scratch copy first so a bad file never leaves a half-filled list behind
        auto* parsed = new PressureProfile[kMaxPressureProfiles];

        if (!jsonError && parsePressureProfiles(doc, parsed, count, selected, error)) {
            for (uint8_t i = 0; i < count; i++) {
                pressureProfiles[i] = parsed[i];
            }

            pressureProfileCount = count;
            selectedPressureProfile = selected;
            delete[] parsed;

            LOGF(INFO, "Loaded %u pressure profiles, active: %s", pressureProfileCount, selectedPressureProfile < 0 ? "simple mode" : pressureProfiles[selectedPressureProfile].name);
            return;
        }

        delete[] parsed;
        LOGF(ERROR, "Invalid pressure profiles file (%s), restoring defaults", error.c_str());
    }

    createDefaultPressureProfiles();
    savePressureProfiles();
}

/**
 * @brief Validate a profiles document received from the web server (any task) and queue it for the main loop
 *
 * @return empty string on success, otherwise the reason it was rejected
 */
inline String queuePressureProfilesUpdate(const String& json) {
    if (json.length() > kPressureProfilesMaxJsonSize) {
        return "Profiles document too large";
    }

    JsonDocument doc;

    if (const DeserializationError jsonError = deserializeJson(doc, json)) {
        return String("Invalid JSON: ") + jsonError.c_str();
    }

    auto* update = new (std::nothrow) PendingPressureProfiles();

    if (update == nullptr) {
        return "Out of memory";
    }

    String error;

    if (!parsePressureProfiles(doc, update->profiles, update->count, update->selected, error)) {
        delete update;
        return error;
    }

    std::lock_guard<std::mutex> lock(pendingPressureProfilesMutex);
    delete pendingPressureProfiles; // a newer update replaces one that was not applied yet
    pendingPressureProfiles = update;

    return "";
}

/**
 * @brief Apply a queued profiles update. Call from the main loop only, and only while no brew is running.
 */
inline void processPendingPressureProfiles() {
    PendingPressureProfiles* update;

    {
        std::lock_guard<std::mutex> lock(pendingPressureProfilesMutex);
        update = pendingPressureProfiles;
        pendingPressureProfiles = nullptr;
    }

    if (update == nullptr) {
        return;
    }

    for (uint8_t i = 0; i < update->count; i++) {
        pressureProfiles[i] = update->profiles[i];
    }

    pressureProfileCount = update->count;
    selectedPressureProfile = update->selected;
    delete update;

    savePressureProfiles();

    LOGF(INFO, "Pressure profiles updated (%u profiles, active: %s)", pressureProfileCount, selectedPressureProfile < 0 ? "simple mode" : pressureProfiles[selectedPressureProfile].name);
}

/**
 * @brief The profile selected for the next brew, nullptr in simple mode
 */
inline const PressureProfile* activePressureProfile() {
    if (selectedPressureProfile < 0 || selectedPressureProfile >= pressureProfileCount || pressureProfiles[selectedPressureProfile].phaseCount == 0) {
        return nullptr;
    }

    return &pressureProfiles[selectedPressureProfile];
}
