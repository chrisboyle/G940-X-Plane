#ifndef G940_PROTOCOL_H
#define G940_PROTOCOL_H

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace g940 {
enum LEDColour { OFF = 0, RED = 1, GREEN = 2, AMBER = RED | GREEN };
typedef std::array<LEDColour, 8> LEDState;
// The G940 uses signed 8-bit spring coefficients; evdev uses these values
// shifted left by eight bits. Keep the native and Linux stiffness consistent.
constexpr unsigned springCoefficients[2] = {80, 112};
constexpr unsigned springMaximums[2] = {0x5000, 0x7000};
constexpr float minimumForceRatio = 0.2f;
struct ForceState {
    // Spring centers are independent of the measured stick position.
    float roll = 0.0f;
    float pitch = 0.0f;
    // Airspeed strength with a ground baseline; zero means invalid flight data.
    float speedRatio = 0.0f;
    // Devices without a native spring need the restoring force explicitly.
    float rollForce = 0.0f;
    float pitchForce = 0.0f;
    // Scale the whole effect during a pause fade, including stiffness.
    float effectScale = 1.0f;
};

inline float clamp(float value, float low, float high) {
    return std::isfinite(value) ? std::max(low, std::min(value, high)) : 0.0f;
}

inline float springGain(unsigned axis) {
    return springCoefficients[axis] / 64.0f;
}

inline float springSaturationRatio(float speedRatio, unsigned axis) {
    return clamp(speedRatio * springGain(axis), 0.0f, 1.0f);
}

inline ForceState calculateForce(float roll, float pitch, float speed,
                                float vneKnots, float alpha,
                                float elevatorTrim, float aileronTrim) {
    const float vne = vneKnots * 0.51444444f;
    // At taxi speeds the reported AoA can be meaningless. Ground resistance
    // is centered; blend aerodynamic trim targets in between 5 and 15 m/s.
    const float airflow = clamp((speed - 5.0f) / 10.0f, 0.0f, 1.0f);
    const float centerWeight = airflow * airflow * (3.0f - 2.0f * airflow);
    const float rollCenter = clamp(aileronTrim * 3.0f * centerWeight, -1.0f, 1.0f);
    const float pitchCenter = clamp((-alpha / 50.0f + elevatorTrim) * 1.5f * centerWeight, -1.0f, 1.0f);
    const float rollForce = rollCenter - clamp(roll, -1.0f, 1.0f) * 3.0f;
    const float pitchForce = pitchCenter - clamp(pitch, -1.0f, 1.0f) * 1.5f;
    const bool validSpeed = std::isfinite(speed) && speed >= 0.0f &&
        std::isfinite(vne) && vne > 0.0f;
    ForceState result{rollCenter, pitchCenter,
        validSpeed ? std::max(minimumForceRatio, clamp(speed / vne, 0.0f, 1.0f)) : 0.0f,
        rollForce, pitchForce};
    return result;
}

// Keep the Linux constant-force fallback's per-axis caps consistent with the
// native spring profile. The backend applies the pause scale after mixing.
inline std::array<float, 2> constantForceComponents(const ForceState& state) {
    std::array<float, 2> force = {{0.0f, 0.0f}};
    const float demands[] = {state.rollForce, state.pitchForce};
    const float ratio = clamp(state.speedRatio, 0.0f, 1.0f);
    for (unsigned axis = 0; axis < force.size(); ++axis) {
        const float cap = springSaturationRatio(ratio, axis) * springMaximums[axis] / 32767.0f;
        const float demand = std::isfinite(demands[axis]) ? demands[axis] : 0.0f;
        force[axis] = clamp(demand * springGain(axis) * ratio, -cap, cap);
    }
    return force;
}

// G940 reports include their report ID. Feature 3 has eight red bits then
// eight green bits. Output 2 has two 30-byte axes and three reserved bytes.
// See tools/PROTOCOL.md for the byte layout.
inline std::array<uint8_t, 3> ledReport(const LEDState& state) {
    std::array<uint8_t, 3> report = {{3, 0, 0}};
    for (unsigned i = 0; i < state.size(); ++i) {
        if (state[i] & RED) report[1] |= 1u << i;
        if (state[i] & GREEN) report[2] |= 1u << i;
    }
    return report;
}

inline void put16(uint8_t *target, int value) {
    const uint16_t bits = static_cast<uint16_t>(value);
    target[0] = bits & 0xff;
    target[1] = bits >> 8;
}

inline std::array<uint8_t, 64> forceReport(const ForceState& state) {
    std::array<uint8_t, 64> report = {{2}};
    const float scale = clamp(state.effectScale, 0.0f, 1.0f);
    if (scale == 0.0f) return report;
    const float centers[] = {state.roll, state.pitch};
    for (unsigned axis = 0; axis < 2; ++axis) {
        uint8_t *data = report.data() + 1 + 30 * axis;
        const int center = clamp(centers[axis], -1.0f, 1.0f) * 0x7fff;
        put16(data + 6, center);
        put16(data + 8, center);
        data[10] = data[11] = springCoefficients[axis] * scale;
        // The firmware's hands-off channel has an 8-bit cap in units of 256.
        // Use that same cap in both modes so releasing the grip keeps the load.
        const float cap = springSaturationRatio(state.speedRatio, axis) * springMaximums[axis] * scale;
        put16(data + 12, std::min(127, static_cast<int>(std::lround(cap / 256.0f))) * 256);
        // The firmware's velocity channel opposes motion for positive
        // coefficients. Light damping helps settle near the trimmed center.
        data[22] = data[23] = 8 * scale;
        put16(data + 24, clamp(state.speedRatio, 0.0f, 1.0f) * 4096 * scale);
    }
    return report;
}
inline std::array<uint8_t, 4> idleForceReport(const std::array<uint8_t, 64>& force, unsigned axis) {
    const uint8_t *data = force.data() + 1 + 30 * axis;
    return {{static_cast<uint8_t>(5 + axis), data[10], data[13], data[22]}};
}
inline std::array<uint8_t, 5> idleCenterReport(const std::array<uint8_t, 64>& force) {
    return {{10, force[7], force[8], force[37], force[38]}};
}
inline std::array<uint8_t, 64> stopReport() {
    std::array<uint8_t, 64> report = {{2}};
    return report;
}
}
#endif
