#include "g940Protocol.h"
#include "g940ForceModel.h"
#include <cassert>
#include <cstdio>
#include <limits>

int main() {
    using namespace g940;
    const LEDState colours = {{RED, GREEN, AMBER, OFF, RED, GREEN, AMBER, OFF}};
    const auto leds = ledReport(colours);
    assert((leds == std::array<uint8_t, 3>{{3, 0x55, 0x66}}));
    const auto force = forceReport({-1.0, 1.0, 1.0});
    assert(force[0] == 2 && force.size() == 64);
    assert(force[7] == 0x01 && force[8] == 0x80); // signed roll center
    assert(force[9] == 0x01 && force[10] == 0x80);
    assert(force[37] == 0xff && force[38] == 0x7f); // pitch center
    assert(force[13] == 0 && force[14] == 0x50); // intermediate roll saturation
    assert(force[43] == 0 && force[44] == 0x70); // reduced pitch saturation
    const auto halfSpeed = forceReport({0, 0, .5});
    assert(halfSpeed[13] == 0 && halfSpeed[14] == 0x32); // 62.5% of roll cap
    assert(halfSpeed[43] == 0 && halfSpeed[44] == 0x62); // 87.5% of pitch cap
    assert(springSaturationRatio(.9, 1) == 1); // strength never exceeds the device limit
    for (unsigned axis = 0; axis < 2; ++axis) {
        assert(force[1 + 30 * axis + 10] == (axis == 0 ? 80 : 112));
        assert(force[1 + 30 * axis + 11] == (axis == 0 ? 80 : 112));
        assert(force[1 + 30 * axis + 22] > 0);
        assert(force[1 + 30 * axis + 23] > 0);
    }
    const auto stop = stopReport();
    assert(stop[0] == 2);
    for (unsigned i = 1; i < stop.size(); ++i) assert(stop[i] == 0);
    assert(forceReport({.5, -.5, 1, 0, 0, 0}) == stop);
    const auto halfEffect = forceReport({0, 0, .5, 0, 0, .5});
    assert(halfEffect[11] == 40 && halfEffect[41] == 56); // stiffness fades too
    assert(halfEffect[43] == 0 && halfEffect[44] == 0x31); // half of pitch cap at half speed
    const auto stationary = calculateForce(0, 0, 0, 100, 0, 0, 0);
    assert(stationary.speedRatio == minimumForceRatio);
    const auto taxi = calculateForce(.2, -.2, .68, 187, -121, .2, .1);
    assert(taxi.speedRatio == minimumForceRatio && taxi.roll == 0 && taxi.pitch == 0);
    assert(taxi.rollForce < 0 && taxi.pitchForce > 0); // mechanical restoring force
    const auto taxiReport = forceReport(taxi);
    assert(taxiReport[14] != 0 && taxiReport[44] != 0);
    const auto blended = calculateForce(0, 0, 10, 187, 5, .2, .1);
    assert(std::abs(blended.roll - .15) < 1e-6);
    assert(std::abs(blended.pitch - .075) < 1e-6);
    assert(calculateForce(0, 0, 100, 0, 0, 0, 0).speedRatio == 0);
    assert(calculateForce(0, 0, -100, 100, 0, 0, 0).speedRatio == 0);
    assert(calculateForce(0, 0, 1000, 100, 0, 0, 0).speedRatio == 1);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    assert(calculateForce(0, 0, nan, 100, 0, 0, 0).speedRatio == 0);
    assert(calculateForce(0, 0, infinity, 100, 0, 0, 0).speedRatio == 0);
    assert(calculateForce(0, 0, 10, infinity, 0, 0, 0).speedRatio == 0);
    const auto invalid = forceReport({nan, nan, nan});
    assert(invalid[7] == 0 && invalid[37] == 0 && invalid[13] == 0);
    assert(forceReport({0, 0, 1, 0, 0, nan}) == stop);

    const auto cappedConstant = constantForceComponents({0, 0, 1, 10, -10});
    assert(std::abs(cappedConstant[0] - 20480.0 / 32767) < 1e-6);
    assert(std::abs(cappedConstant[1] + 28672.0 / 32767) < 1e-6);
    const auto smallConstant = constantForceComponents({0, 0, .5, .1, .1});
    assert(std::abs(smallConstant[0] - .0625) < 1e-6);
    assert(std::abs(smallConstant[1] - .0875) < 1e-6);
    const auto invalidConstant = constantForceComponents({0, 0, nan, infinity, nan});
    assert(invalidConstant[0] == 0 && invalidConstant[1] == 0);

    assert((idleCenterReport(force) == std::array<uint8_t, 5>{{10, 1, 128, 255, 127}}));
    assert((idleForceReport(force, 0) == std::array<uint8_t, 4>{{5, 80, 80, 8}}));
    assert((idleForceReport(force, 1) == std::array<uint8_t, 4>{{6, 112, 112, 8}}));
    for (int i = 0; i <= 100; ++i) {
        const auto sample = forceReport({.1, -.3, i / 100.0f, 0, 0, .6});
        for (unsigned axis = 0; axis < 2; ++axis) {
            const auto idle = idleForceReport(sample, axis);
            const unsigned start = 1 + 30 * axis;
            assert(sample[start + 12] == 0); // same /256 cap in both grip modes
            assert(idle[1] == sample[start + 10] && idle[2] == sample[start + 13]);
            assert(idle[3] == sample[start + 22]);
            assert(idle[2] < 128); // positive signed firmware saturation
        }
    }

    // Stick motion must not move the spring's trim target along with the
    // physical stick: doing so creates feedback around a moving neutral point.
    const auto trimTarget = calculateForce(0, 0, 25.722222, 100, 0, .5, .1);
    const auto displaced = calculateForce(.5, .5, 25.722222, 100, 0, .5, .1);
    assert(trimTarget.roll == displaced.roll && trimTarget.pitch == displaced.pitch);
    assert(trimTarget.rollForce > displaced.rollForce);
    assert(trimTarget.pitchForce > displaced.pitchForce);
    assert(displaced.pitchForce == 0); // held at the pitch trim target
    const auto untrimmed = calculateForce(0, .5, 25.722222, 100, 0, 0, 0);
    assert(untrimmed.pitchForce < displaced.pitchForce);

    ForceSmoother smoother;
    ForceState previous;
    for (int i = 0; i < 250; ++i) {
        const auto next = smoother.update(trimTarget, .02);
        assert(next.pitch >= previous.pitch);
        assert(next.pitch - previous.pitch <= .005001);
        assert(next.speedRatio - previous.speedRatio <= .020001);
        previous = next;
    }
    assert(std::abs(previous.pitch - trimTarget.pitch) < 1e-4);
    assert(std::abs(previous.speedRatio - trimTarget.speedRatio) < 1e-6);
    const auto reverseTrim = calculateForce(0, 0, 25.722222, 100, 0, -.5, -.1);
    const auto afterLongFrame = smoother.update(reverseTrim, 100);
    assert(previous.pitch - afterLongFrame.pitch <= .025001);
    const auto invalidVne = calculateForce(0, 0, 10, 0, 0, 0, 0);
    const auto invalidState = smoother.update(invalidVne, .02);
    assert(invalidState.speedRatio == 0); // invalid data stops immediately, even during a ramp
    smoother.reset();
    const auto restarted = smoother.update(trimTarget, .02);
    assert(restarted.pitch <= .005001 && restarted.speedRatio <= .020001);
    smoother.reset(trimTarget);
    const auto primed = smoother.update(trimTarget, .02f);
    assert(primed.pitch == trimTarget.pitch && primed.roll == trimTarget.roll);
    assert(primed.speedRatio <= .020001f && primed.effectScale <= .020001f);
    assert(std::abs(primed.pitchForce - trimTarget.pitchForce) < 1e-6f);

    // Small trim steps must be filtered as well as large changes: the old
    // slew limiter applied a small step in one frame.
    ForceSmoother smallTrim;
    const ForceState smallTarget{0.0f, .004f, .5f};
    const auto firstSmallStep = smallTrim.update(smallTarget, .02);
    assert(firstSmallStep.pitch > 0 && firstSmallStep.pitch < .001);
    assert(firstSmallStep.pitch < smallTarget.pitch);
    auto settled = firstSmallStep;
    for (int i = 0; i < 100; ++i) settled = smallTrim.update(smallTarget, .02);
    assert(std::abs(settled.pitch - smallTarget.pitch) < 1e-5);
    assert(settled.pitch <= smallTarget.pitch); // no overshoot

    double lastScale = 1.0;
    for (int i = 0; i < 25; ++i) {
        settled = smallTrim.release(.02);
        assert(settled.effectScale <= lastScale && settled.effectScale >= 0);
        assert(lastScale - settled.effectScale <= .031);
        lastScale = settled.effectScale;
    }
    assert(std::abs(settled.effectScale - .5) < 1e-6);
    const auto resumed = smallTrim.update(smallTarget, .02);
    assert(resumed.effectScale > settled.effectScale);
    assert(resumed.effectScale - settled.effectScale <= .020001);
    assert(!smallTrim.releasing());
    const auto stopped = smallTrim.release(5); // expired pause after a long frame
    assert(stopped.effectScale == 0 && forceReport(stopped) == stop);
    smallTrim.reset();
    assert(!smallTrim.releasing());
    std::puts("Force packets, stable trim targets, and bounded transitions passed.");
}
