#include "g940Backend.h"
#include "g940HID.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#if LIN
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace g940 {
namespace {
std::string lastError;
bool ledsOpen = false;
bool haveLEDState = false;
LEDState previousLEDs = {{OFF}};
#if LIN
int forceFD = -1;
ff_effect effect = {};
bool haveSpring = true;
void systemError(const std::string& operation) {
    lastError = operation + ": " + std::strerror(errno);
}
bool writeLED(unsigned label, const char *component, bool on) {
    char path[128];
    std::snprintf(path, sizeof(path), "/sys/class/leds/g940:%s:P%u/brightness", component, label);
    int file = open(path, O_WRONLY);
    if (file < 0) { systemError(path); return false; }
    const bool success = write(file, on ? "1" : "0", 1) == 1;
    if (!success) systemError(path);
    close(file);
    return success;
}
bool tryJoystick(const char *path) {
    const int candidate = open(path, O_RDWR | O_NONBLOCK);
    if (candidate < 0) { systemError(path); return false; }
    // Only try evdev devices that explicitly advertise force feedback.
    unsigned long bits[(FF_MAX + 8 * sizeof(unsigned long)) / (8 * sizeof(unsigned long))] = {};
    if (ioctl(candidate, EVIOCGBIT(EV_FF, sizeof(bits)), bits) < 0) { close(candidate); return false; }
    const auto supports = [&](unsigned type) {
        return (bits[type / (8 * sizeof(unsigned long))] >> (type % (8 * sizeof(unsigned long)))) & 1;
    };
    if (!supports(FF_SPRING) && !supports(FF_CONSTANT)) { close(candidate); return false; }
    effect = ff_effect();
    effect.id = -1;
    haveSpring = supports(FF_SPRING);
    effect.type = haveSpring ? FF_SPRING : FF_CONSTANT;
    if (ioctl(candidate, EVIOCSFF, &effect) < 0) {
        haveSpring = false;
        effect.type = FF_CONSTANT;
        if (!supports(FF_CONSTANT) || ioctl(candidate, EVIOCSFF, &effect) < 0) {
            systemError(std::string(path) + " upload effect"); close(candidate); return false;
        }
    }
    input_event play = {};
    play.type = EV_FF; play.code = effect.id; play.value = 1;
    if (write(candidate, &play, sizeof(play)) != sizeof(play)) {
        systemError("Play effect"); close(candidate); return false;
    }
    forceFD = candidate;
    return true;
}
#else
HIDDevice forceDevice;
HIDDevice ledDevice;
std::array<uint8_t, 3> originalLEDs = {{3, 0, 0}};
bool haveOriginalLEDs = false;
// Own a feature's original value and last verified write. Keep the backup
// across pauses and restore it even after a partially applied transfer.
template<size_t Size>
struct SavedFeature {
    const uint8_t id;
    std::array<uint8_t, Size> original, current;
    SavedFeature(uint8_t id) : id(id), original{id}, current{id} {}
    bool backup() {
        original[0] = id;
        if (!forceDevice.getFeature(original)) { lastError = forceDevice.error(); return false; }
        if (original[0] != id) { lastError = "Unexpected G940 idle feature report ID"; return false; }
        current = original;
        return true;
    }
    bool write(const std::array<uint8_t, Size>& value) {
        std::array<uint8_t, Size> verified = {id};
        if (!forceDevice.setFeature(value) || !forceDevice.getFeature(verified)) {
            lastError = forceDevice.error(); return false;
        }
        if (verified != value) { lastError = "G940 idle settings did not read back correctly"; return false; }
        current = value;
        return true;
    }
    bool update(const std::array<uint8_t, Size>& value) { return value == current || write(value); }
};
std::array<SavedFeature<4>, 2> idleSettings = {{{5}, {6}}};
SavedFeature<5> idleCenters{10};
bool haveOriginalIdle = false;
#endif
}

const char *backendError() { return lastError.c_str(); }

bool prepareForceFeedback() {
#if LIN
    return true;
#else
    if (forceDevice.isOpen()) return true;
    if (!forceDevice.open()) { lastError = forceDevice.error(); return false; }
    // Back up every feature before modifying any of them.
    for (auto& feature : idleSettings) {
        if (!feature.backup()) { forceDevice.close(); return false; }
    }
    if (!idleCenters.backup()) { forceDevice.close(); return false; }
    haveOriginalIdle = true;
    if (!releaseForceFeedback()) {
        const std::string error = lastError;
        closeForceFeedback(); lastError = error; return false;
    }
    return true;
#endif
}

bool openForceFeedback() {
#if LIN
    if (forceFD >= 0) return true;
    lastError = "No writable evdev force-feedback device found in /dev/input";
    DIR *directory = opendir("/dev/input");
    if (!directory) { systemError("Open /dev/input"); return false; }
    while (dirent *entry = readdir(directory)) {
        if (std::strncmp(entry->d_name, "event", 5) != 0) continue;
        const std::string path = std::string("/dev/input/") + entry->d_name;
        if (tryJoystick(path.c_str())) break;
    }
    closedir(directory);
    return forceFD >= 0;
#else
    return prepareForceFeedback();
#endif
}

bool updateForceFeedback(const ForceState& state) {
#if LIN
    if (forceFD < 0) return false;
    const float scale = clamp(state.effectScale, 0.0f, 1.0f);
    if (haveSpring) {
        const float centers[] = {state.roll, state.pitch};
        for (unsigned axis = 0; axis < 2; ++axis) {
            auto& condition = effect.u.condition[axis];
            condition.center = clamp(centers[axis], -1.0f, 1.0f) * 0x7fff;
            condition.left_coeff = condition.right_coeff = (springCoefficients[axis] << 8) * scale;
            condition.left_saturation = condition.right_saturation =
                springSaturationRatio(state.speedRatio, axis) * (2 * springMaximums[axis]) * scale;
        }
    } else {
        const auto components = constantForceComponents(state);
        const float roll = components[0], pitch = components[1];
        effect.u.constant.level = clamp(std::hypot(roll, pitch), 0.0f, 1.0f) * 0x7fff * scale;
        const float angle = std::atan2(-roll, pitch);
        const int direction = angle * 32768.0f / std::acos(-1.0f);
        effect.direction = static_cast<uint16_t>(direction);
    }
    if (ioctl(forceFD, EVIOCSFF, &effect) >= 0) return true;
    systemError("Update effect");
    const std::string updateError = lastError;
    closeForceFeedback();
    lastError = updateError;
#else
    bool success;
    if (state.speedRatio <= 0.0f || !std::isfinite(state.speedRatio) ||
        clamp(state.effectScale, 0.0f, 1.0f) == 0.0f) {
        success = releaseForceFeedback();
    } else {
        const auto report = forceReport(state);
        // Update the idle center first: when starting, its force is still zero.
        // Cache unchanged features to avoid unnecessary control transfers.
        success = idleCenters.update(idleCenterReport(report));
        for (unsigned axis = 0; axis < idleSettings.size() && success; ++axis)
            success = idleSettings[axis].update(idleForceReport(report, axis));
        if (success && !forceDevice.setOutput(report)) {
            lastError = forceDevice.error(); success = false;
        }
    }
    if (success) return true;
    const std::string updateError = lastError;
    closeForceFeedback();
    lastError = updateError;
#endif
    return false;
}

bool releaseForceFeedback() {
#if LIN
    if (forceFD < 0) return true;
    input_event stop = {};
    stop.type = EV_FF; stop.code = effect.id; stop.value = 0;
    bool success = write(forceFD, &stop, sizeof(stop)) == sizeof(stop);
    if (!success) systemError("Stop effect");
    if (ioctl(forceFD, EVIOCRMFF, effect.id) < 0) {
        systemError("Remove effect"); success = false;
    }
    close(forceFD);
    forceFD = -1;
    return success;
#else
    bool success = true;
    if (forceDevice.isOpen()) {
        const auto report = stopReport();
        if (!forceDevice.setOutput(report)) {
            lastError = forceDevice.error(); success = false;
        }
        // Pause/error stops must remove hands-off force as well as live force.
        for (auto& feature : idleSettings) {
            if (!feature.write({feature.id, 0, 0, 0})) success = false;
        }
    }
    return success;
#endif
}

bool closeForceFeedback() {
    bool success = releaseForceFeedback();
#if !LIN
    if (forceDevice.isOpen() && haveOriginalIdle) {
        // Restore the original neutral positions while idle force is zero.
        if (!idleCenters.write(idleCenters.original)) success = false;
        // Always attempt both restores, even if stopping or the first restore failed.
        for (auto& feature : idleSettings) {
            if (!feature.write(feature.original)) success = false;
        }
    }
    forceDevice.close();
    haveOriginalIdle = false;
#endif
    return success;
}

bool openLEDs() {
    if (ledsOpen) return true;
#if LIN
    if (access("/sys/class/leds/g940:red:P1/brightness", W_OK) != 0) {
        systemError("G940 LED sysfs access (driver patches and write permission required)");
        return false;
    }
#else
    if (!ledDevice.open()) { lastError = ledDevice.error(); return false; }
    originalLEDs = {{3, 0, 0}};
    haveOriginalLEDs = ledDevice.getFeature(originalLEDs);
    if (!haveOriginalLEDs) {
        lastError = ledDevice.error(); ledDevice.close(); return false;
    }
#endif
    ledsOpen = true;
    haveLEDState = false;
    return true;
}

bool updateLEDs(const LEDState& state) {
    if (!ledsOpen) return false;
#if LIN
    if (haveLEDState && state == previousLEDs) return true;
    for (unsigned i = 0; i < state.size(); ++i) {
        if (haveLEDState && previousLEDs[i] == state[i]) continue;
        if (!writeLED(i + 1, "red", state[i] & RED) || !writeLED(i + 1, "green", state[i] & GREEN)) {
            ledsOpen = false; haveLEDState = false; return false;
        }
    }
#else
    // Refresh even unchanged colours: a write detects a removed HID device
    // and repopulates the LEDs after a reconnect. The callback runs at 5 Hz.
    const auto report = ledReport(state);
    if (!ledDevice.setFeature(report)) {
        lastError = ledDevice.error();
        ledDevice.close(); ledsOpen = false; haveLEDState = false; return false;
    }
#endif
    previousLEDs = state;
    haveLEDState = true;
    return true;
}

void closeLEDs() {
    if (!ledsOpen) return;
#if LIN
    LEDState green;
    green.fill(GREEN);
    updateLEDs(green);
#else
    if (haveOriginalLEDs && !ledDevice.setFeature(originalLEDs))
        lastError = ledDevice.error();
    ledDevice.close();
    haveOriginalLEDs = false;
#endif
    ledsOpen = false;
    haveLEDState = false;
}
}
