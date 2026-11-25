/**
 * @file jsonPath.h
 *
 * @brief Allocation free lookup of a dot separated path (e.g. "pid.regular.kp") in a JSON document.
 *
 *        Config::get() is called dozens of times per loop() iteration. Splitting the path into String segments
 *        allocates on the heap for every call, here the segments are passed to ArduinoJson as sized views into the
 *        path instead.
 */

#pragma once

#include <ArduinoJson.h>
#include <cstring>

/**
 * @brief Resolve a dot separated path
 *
 * @param root object the path starts at
 * @param path dot separated keys, null terminated
 * @param leaf the value at the end of the path; unbound if the last key does not exist
 * @return false if the path is empty, ends with a dot or one of its parent objects does not exist (leaf untouched)
 */
inline bool resolveJsonPath(JsonVariantConst root, const char* path, JsonVariantConst& leaf) {
    if (path == nullptr) {
        return false;
    }

    JsonVariantConst current = root;
    const char* segment = path;

    while (const char* dot = strchr(segment, '.')) {
        current = current[JsonString(segment, static_cast<size_t>(dot - segment))];

        if (current.isNull()) {
            return false;
        }

        segment = dot + 1;
    }

    if (*segment == '\0' || current.isNull()) {
        return false;
    }

    leaf = current[JsonString(segment, strlen(segment))];

    return true;
}
