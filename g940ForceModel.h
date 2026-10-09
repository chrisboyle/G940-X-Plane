#ifndef G940_FORCE_MODEL_H
#define G940_FORCE_MODEL_H

#include "g940Protocol.h"

namespace g940 {
class ForceSmoother {
public:
    void reset() {
        state_ = ForceState();
        releasing_ = false;
        releaseElapsed_ = 0.0f;
    }
    void reset(const ForceState& target) {
        reset();
        // Establish the current trim position while force is zero, rather
        // than pulling toward an artificial zero center after connecting.
        state_ = target;
        state_.speedRatio = 0.0f;
        state_.effectScale = 0.0f;
    }
    bool releasing() const { return releasing_; }
    ForceState update(const ForceState& target, float elapsed) {
        // A long simulator frame must not turn a trim change into one jump.
        const float dt = clamp(elapsed, 0.0f, 0.1f);
        releasing_ = false;
        // Resuming partway through a release must not jump to full strength.
        state_.effectScale = approach(state_.effectScale, 1.0f, dt);
        state_.roll = approach(state_.roll, clamp(target.roll, -1.0f, 1.0f), 0.5f * dt);
        // Filter small trim/AoA changes too, and taper into the new center.
        // The lower slew rate also offsets the stronger pitch spring.
        const float pitchTarget = clamp(target.pitch, -1.0f, 1.0f);
        const float filteredPitch = state_.pitch +
            (pitchTarget - state_.pitch) * (-std::expm1(-dt / 0.25f));
        state_.pitch = approach(state_.pitch, filteredPitch, 0.25f * dt);
        const float ratio = clamp(target.speedRatio, 0.0f, 1.0f);
        state_.speedRatio = ratio == 0.0f ? 0.0f : approach(state_.speedRatio, ratio, dt);
        // Keep immediate stick-dependent restoring force on the Linux
        // constant-force fallback, using the same smoothed trim centers.
        state_.rollForce = target.rollForce + state_.roll - target.roll;
        state_.pitchForce = target.pitchForce + state_.pitch - target.pitch;
        return state_;
    }
    ForceState release(float elapsed) {
        if (!releasing_) {
            releasing_ = true;
            releaseElapsed_ = 0.0f;
            releaseStartScale_ = state_.effectScale;
        }
        // Honor the one-second release even after a delayed callback.
        releaseElapsed_ = clamp(releaseElapsed_ + clamp(elapsed, 0.0f, 1.0f), 0.0f, 1.0f);
        if (releaseElapsed_ > 1.0f - 1e-6f) releaseElapsed_ = 1.0f;
        const float t = releaseElapsed_;
        state_.effectScale = releaseStartScale_ * (1.0f - t * t * (3.0f - 2.0f * t));
        return state_;
    }
private:
    static float approach(float current, float target, float step) {
        return current + clamp(target - current, -step, step);
    }
    ForceState state_;
    bool releasing_ = false;
    float releaseElapsed_ = 0.0f;
    float releaseStartScale_ = 1.0f;
};
}
#endif
