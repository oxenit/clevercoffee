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
constexpr const char* kPressureProfilesTempPath = "/profiles.json.tmp";
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

// held by the main loop while it replaces the live list, and by the web server while it sends it
inline std::mutex pressureProfilesMutex;
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
 * @brief Parse and validate one profile object
 */
inline bool parsePressureProfile(JsonObjectConst profileJson, PressureProfile& profile, const uint8_t index, String& error) {
    profile = PressureProfile();

    const char* name = profileJson["label"] | "";

    if (strlen(name) == 0) {
        error = String("Profile ") + (index + 1) + " has no label";
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

    if (profile.totalDuration() <= 0.0f) {
        error = String("Profile '") + profile.name + "' has a total duration of 0 s";
        return false;
    }

    return true;
}

/**
 * @brief Reader over a memory buffer that keeps its position between deserializeJson() calls, so the profiles
 *        document can be parsed one profile at a time. ArduinoJson reads from it byte by byte and stops right
 *        after the value it parsed.
 */
class ProfilesJsonReader {
    public:
        ProfilesJsonReader(const char* data, const size_t length) :
            pos_(data), end_(data + length) {
        }

        int read() {
            return pos_ < end_ ? static_cast<unsigned char>(*pos_++) : -1;
        }

        size_t readBytes(char* buffer, const size_t length) {
            size_t count = 0;

            while (count < length && pos_ < end_) {
                buffer[count++] = *pos_++;
            }

            return count;
        }

        /**
         * @brief Skip whitespace and return the next character without consuming it, 0 at the end
         */
        char peek() {
            while (pos_ < end_ && (*pos_ == ' ' || *pos_ == '\t' || *pos_ == '\n' || *pos_ == '\r')) {
                pos_++;
            }

            return pos_ < end_ ? *pos_ : '\0';
        }

        /**
         * @brief Consume the next non-whitespace character if it is the expected one
         */
        bool consume(const char expected) {
            if (peek() != expected) {
                return false;
            }

            pos_++;

            return true;
        }

    private:
        const char* pos_;
        const char* end_;
};

/**
 * @brief Parse and validate a profiles document
 *
 *        A full document (8 profiles x 8 phases with exit targets, ~25 kB of JSON) needs more than 50 kB as a
 *        JsonDocument, which the ESP32 cannot spare next to the request body while WiFi is running. So the document
 *        is never loaded as a whole: a filtered pass checks the syntax and reads "selected", then the "profiles"
 *        array is walked with one small JsonDocument per profile.
 *
 * @param json document text
 * @param length length of the text
 * @param profiles output array, kMaxPressureProfiles entries
 * @param count number of parsed profiles
 * @param selected parsed selection
 * @param error human readable reason when the document is rejected
 * @return true if the document is valid
 */
inline bool parsePressureProfiles(const char* json, const size_t length, PressureProfile* profiles, uint8_t& count, int8_t& selected, String& error) {
    count = 0;

    // pass 1: syntax check of the whole document, keeping only "selected"
    JsonDocument filter;
    filter["selected"] = true;
    JsonDocument header;

    if (const DeserializationError jsonError = deserializeJson(header, json, length, DeserializationOption::Filter(filter))) {
        error = String("Invalid JSON: ") + jsonError.c_str();
        return false;
    }

    if (!header.is<JsonObjectConst>()) {
        error = "'profiles' must be an array";
        return false;
    }

    // pass 2: walk the top level members up to the "profiles" array (the syntax is known to be valid here)
    ProfilesJsonReader reader(json, length);
    JsonDocument skipAll;
    skipAll.set(false); // filter that drops everything: deserializeJson() then just consumes one value
    bool found = false;

    reader.consume('{');

    while (reader.peek() == '"') {
        JsonDocument key;
        deserializeJson(key, reader);

        reader.consume(':');

        const char* keyName = key.as<const char*>();

        if (keyName != nullptr && strcmp(keyName, "profiles") == 0) {
            found = reader.consume('[');
            break;
        }

        JsonDocument skipped;
        deserializeJson(skipped, reader, DeserializationOption::Filter(skipAll));

        reader.consume(',');
    }

    if (!found) {
        error = "'profiles' must be an array";
        return false;
    }

    // pass 3: one profile at a time
    while (reader.peek() != ']') {
        if (count >= kMaxPressureProfiles) {
            error = String("At most ") + kMaxPressureProfiles + " profiles are supported";
            return false;
        }

        JsonDocument doc;

        if (const DeserializationError jsonError = deserializeJson(doc, reader)) {
            error = jsonError == DeserializationError::NoMemory ? String("Out of memory") : String("Invalid JSON: ") + jsonError.c_str();
            return false;
        }

        if (doc.overflowed()) {
            error = "Out of memory";
            return false;
        }

        if (!parsePressureProfile(doc.as<JsonObjectConst>(), profiles[count], count, error)) {
            return false;
        }

        count++;
        reader.consume(',');
    }

    const int selectedIndex = header["selected"] | -1;

    if (selectedIndex < -1 || selectedIndex >= count) {
        error = "'selected' must be -1 (simple mode) or the index of an existing profile";
        return false;
    }

    selected = static_cast<int8_t>(selectedIndex);

    return true;
}

inline void serializePressureProfile(const PressureProfile& profile, JsonDocument& doc) {
    doc["label"] = profile.name;
    doc["type"] = "pro";
    const JsonArray phases = doc["phases"].to<JsonArray>();

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

template <typename Writer>
bool writeProfilesLiteral(Writer& out, const char* text) {
    const size_t length = strlen(text);
    return out.write(reinterpret_cast<const uint8_t*>(text), length) == length;
}

/**
 * @brief Serialize one profile of the live list
 *
 * @return false if the index is out of range or there is not enough memory
 */
inline bool pressureProfileToJson(const uint8_t index, String& out) {
    out = "";

    if (index >= pressureProfileCount) {
        return false;
    }

    JsonDocument doc;
    serializePressureProfile(pressureProfiles[index], doc);

    if (doc.overflowed()) {
        return false;
    }

    const size_t length = measureJson(doc);

    return out.reserve(length + 1) && serializeJson(doc, out) == length;
}

/**
 * @brief The members "selected" and "profiles" up to the opening bracket of the list
 */
inline String pressureProfilesJsonHeader() {
    char header[40];
    snprintf(header, sizeof(header), "\"selected\":%d,\"profiles\":[", selectedPressureProfile);

    return header;
}

/**
 * @brief Write the members "selected" and "profiles" plus the closing brace of the profiles document, one profile
 *        at a time so the full list (~25 kB of JSON) is never held in memory. The caller writes the opening brace.
 *
 * @param out anything with write(const uint8_t*, size_t), e.g. a File
 * @return false if a profile could not be serialized (out of memory) or a write came up short (filesystem full),
 *         the output is incomplete then
 */
template <typename Writer>
bool writePressureProfilesBody(Writer& out) {
    if (!writeProfilesLiteral(out, pressureProfilesJsonHeader().c_str())) {
        return false;
    }

    String text;

    for (uint8_t i = 0; i < pressureProfileCount; i++) {
        if (!pressureProfileToJson(i, text) || (i > 0 && !writeProfilesLiteral(out, ",")) || !writeProfilesLiteral(out, text.c_str())) {
            return false;
        }
    }

    return writeProfilesLiteral(out, "]}");
}

/**
 * @brief Produces the profiles document piece by piece for a chunked HTTP response: a 25 kB response buffer cannot be
 *        allocated reliably while WiFi is running (and the web server aborts when that allocation fails), so only one
 *        profile is serialized at a time. The live list is locked while a profile is read; an update applied between
 *        two pieces can at worst produce a mix of the old and new list, which the next reload corrects.
 */
class PressureProfilesJsonStream {
    public:
        /**
         * @param prefix opening brace and any members before "selected", e.g. {"maxProfiles":8,
         */
        explicit PressureProfilesJsonStream(const String& prefix) :
            piece_(prefix) {
        }

        /**
         * @brief Copy the next part of the document
         *
         * @return number of bytes written to buffer, 0 at the end (or when out of memory, the JSON is cut off then)
         */
        size_t read(uint8_t* buffer, const size_t maxLength) {
            while (offset_ >= piece_.length()) {
                if (!nextPiece()) {
                    return 0;
                }
            }

            size_t length = piece_.length() - offset_;

            if (length > maxLength) {
                length = maxLength;
            }

            memcpy(buffer, piece_.c_str() + offset_, length);
            offset_ += length;

            return length;
        }

    private:
        bool nextPiece() {
            if (stage_ == Stage::Closing || stage_ == Stage::Done) {
                piece_ = "";
                offset_ = 0;
                stage_ = Stage::Done;
                return false;
            }

            std::lock_guard<std::mutex> lock(pressureProfilesMutex);
            offset_ = 0;

            if (stage_ == Stage::Header) {
                piece_ = pressureProfilesJsonHeader();
                stage_ = Stage::Profiles;
                return true;
            }

            if (stage_ == Stage::Profiles && index_ < pressureProfileCount) {
                String profile;

                if (!pressureProfileToJson(index_, profile)) {
                    piece_ = "";
                    stage_ = Stage::Done;
                    return false;
                }

                piece_ = index_ > 0 ? "," : "";
                piece_ += profile;
                index_++;

                return true;
            }

            piece_ = "]}";
            stage_ = Stage::Closing;

            return true;
        }

        enum class Stage : uint8_t {
            Header,
            Profiles,
            Closing,
            Done,
        };

        String piece_;
        size_t offset_ = 0;
        Stage stage_ = Stage::Header;
        uint8_t index_ = 0;
};

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

/**
 * @brief Persist the profiles. Written to a temporary file that is then renamed over the old one, so a power cut or a
 *        full filesystem during the write never leaves a truncated file (which would be replaced by the defaults).
 */
inline bool savePressureProfiles() {
    File file = LittleFS.open(kPressureProfilesTempPath, "w");

    if (!file) {
        LOG(ERROR, "Failed to open pressure profiles file for writing");
        return false;
    }

    const bool complete = writeProfilesLiteral(file, "{") && writePressureProfilesBody(file);
    file.close();

    if (!complete || !LittleFS.rename(kPressureProfilesTempPath, kPressureProfilesPath)) {
        LOG(ERROR, "Failed to write pressure profiles file, keeping the previous one");
        LittleFS.remove(kPressureProfilesTempPath);
        return false;
    }

    return true;
}

/**
 * @brief Load the profiles at boot, (re)creating the defaults when the file is missing or invalid
 */
inline void loadPressureProfiles() {
    // power cut between writing the new file and renaming it: the temporary file is complete, use it
    if (!LittleFS.exists(kPressureProfilesPath) && LittleFS.exists(kPressureProfilesTempPath)) {
        LittleFS.rename(kPressureProfilesTempPath, kPressureProfilesPath);
    }

    if (LittleFS.exists(kPressureProfilesPath)) {
        String error = "cannot read file";
        File file = LittleFS.open(kPressureProfilesPath, "r");
        const size_t size = file ? file.size() : 0;

        // parse into a scratch copy first so a bad file never leaves a half-filled list behind
        auto* parsed = new (std::nothrow) PendingPressureProfiles();
        char* content = size > 0 && size <= kPressureProfilesMaxJsonSize ? static_cast<char*>(malloc(size)) : nullptr;
        const bool read = parsed != nullptr && content != nullptr && file.readBytes(content, size) == size;

        if (file) {
            file.close();
        }

        const bool valid = read && parsePressureProfiles(content, size, parsed->profiles, parsed->count, parsed->selected, error);
        free(content);

        if (valid) {
            for (uint8_t i = 0; i < parsed->count; i++) {
                pressureProfiles[i] = parsed->profiles[i];
            }

            pressureProfileCount = parsed->count;
            selectedPressureProfile = parsed->selected;
            delete parsed;

            LOGF(INFO, "Loaded %u pressure profiles, active: %s", pressureProfileCount, selectedPressureProfile < 0 ? "simple mode" : pressureProfiles[selectedPressureProfile].name);
            return;
        }

        delete parsed;
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

    auto* update = new (std::nothrow) PendingPressureProfiles();

    if (update == nullptr) {
        return "Out of memory";
    }

    String error;

    if (!parsePressureProfiles(json.c_str(), json.length(), update->profiles, update->count, update->selected, error)) {
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

    {
        std::lock_guard<std::mutex> lock(pressureProfilesMutex);

        for (uint8_t i = 0; i < update->count; i++) {
            pressureProfiles[i] = update->profiles[i];
        }

        pressureProfileCount = update->count;
        selectedPressureProfile = update->selected;
    }

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
