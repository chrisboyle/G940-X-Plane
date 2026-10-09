#include <cstdio>
#include <cstring>
#include <string>
#ifdef G940_DEBUG_FORCE
#include <chrono>
#endif
#include "XPLMPlugin.h"
#include "XPLMDataAccess.h"
#include "XPLMProcessing.h"
#include "XPLMUtilities.h"
#include "g940Backend.h"
#include "g940ForceModel.h"

#ifndef XPLM300
#error This plugin requires the XPLM300 API
#endif

namespace {
XPLMDataRef rollRef, pitchRef, speedRef, vneRef, alphaRef, eTrimRef, aTrimRef, pausedRef;
bool enabled = false;
bool forceReady = false;
g940::ForceSmoother forceSmoother;
#ifdef G940_DEBUG_FORCE
std::chrono::steady_clock::time_point nextForceTrace;
#endif

void reportError() {
    const std::string message = std::string("G940 FF: ") + g940::backendError() + "\n";
    XPLMDebugString(message.c_str());
}

float flightLoopCallback(float elapsed, float, int, void *) {
    if (!g940::prepareForceFeedback()) {
        reportError(); forceReady = false; forceSmoother.reset(); return 5.0f;
    }
    if (XPLMGetDatai(pausedRef)) {
        if (!forceReady) return 0.2f;
#ifdef G940_DEBUG_FORCE
        const bool startingRelease = !forceSmoother.releasing();
#endif
        const g940::ForceState state = forceSmoother.release(elapsed);
        if (state.speedRatio <= 0.0f || state.effectScale <= 0.0f) {
            if (!g940::releaseForceFeedback()) {
                reportError(); g940::closeForceFeedback();
            }
            forceReady = false;
            forceSmoother.reset();
#ifdef G940_DEBUG_FORCE
            XPLMDebugString("G940 FF trace: paused force released\n");
#endif
            return 0.2f;
        }
#ifdef G940_DEBUG_FORCE
        if (startingRelease) XPLMDebugString("G940 FF trace: one-second pause fade started\n");
#endif
        if (!g940::updateForceFeedback(state)) {
            reportError(); forceReady = false; forceSmoother.reset(); return 0.2f;
        }
        return 0.02f;
    }
    const bool startingForce = !forceReady;
    if (startingForce) {
        if (!g940::openForceFeedback()) { reportError(); return 5.0f; }
        forceReady = true;
        forceSmoother.reset();
#ifdef G940_DEBUG_FORCE
        nextForceTrace = std::chrono::steady_clock::time_point();
#endif
        XPLMDebugString("G940 FF: force-feedback device connected\n");
    }
    const float roll = XPLMGetDataf(rollRef), pitch = XPLMGetDataf(pitchRef);
    const float speed = XPLMGetDataf(speedRef), vne = XPLMGetDataf(vneRef);
    const float alpha = XPLMGetDataf(alphaRef);
    const float elevatorTrim = XPLMGetDataf(eTrimRef), aileronTrim = XPLMGetDataf(aTrimRef);
    const g940::ForceState target = g940::calculateForce(
        roll, pitch, speed, vne, alpha, elevatorTrim, aileronTrim);
    if (startingForce) forceSmoother.reset(target);
    const g940::ForceState state = forceSmoother.update(target, elapsed);
#ifdef G940_DEBUG_FORCE
    const auto now = std::chrono::steady_clock::now();
    if (now >= nextForceTrace) {
        char message[384];
        std::snprintf(message, sizeof(message),
            "G940 FF trace: TAS=%.2f m/s Vne=%.2f kt ratio=%.3f "
            "yoke=(%.3f,%.3f) trim=(%.3f,%.3f) alpha=%.2f centers=(%.3f,%.3f) "
            "spring=(%.3f,%.3f) scale=%.3f\n",
            speed, vne, state.speedRatio, roll, pitch, aileronTrim, elevatorTrim,
            alpha, state.roll, state.pitch,
            g940::springSaturationRatio(state.speedRatio, 0) * state.effectScale,
            g940::springSaturationRatio(state.speedRatio, 1) * state.effectScale,
            state.effectScale);
        XPLMDebugString(message);
        nextForceTrace = now + std::chrono::seconds(2);
    }
#endif
    if (!g940::updateForceFeedback(state)) {
        reportError(); forceReady = false; return 5.0f;
    }
    return 0.02f;
}
}

PLUGIN_API int XPluginStart(char *outName, char *outSig, char *outDesc) {
    std::strcpy(outName, "G940 Force Feedback");
    std::strcpy(outSig, "name.boyle.chris.xpff");
    std::strcpy(outDesc, "Connects X-Plane to force-feedback hardware.");
    struct Reference { XPLMDataRef *target; const char *name; };
    const Reference references[] = {
        {&rollRef, "sim/joystick/yoke_roll_ratio"},
        {&pitchRef, "sim/joystick/yoke_pitch_ratio"},
        {&speedRef, "sim/flightmodel/position/true_airspeed"},
        {&vneRef, "sim/aircraft/view/acf_Vne"},
        {&alphaRef, "sim/flightmodel/position/alpha"},
        {&eTrimRef, "sim/flightmodel2/controls/elevator_trim"},
        {&aTrimRef, "sim/flightmodel2/controls/aileron_trim"},
        {&pausedRef, "sim/time/paused"}
    };
    for (const auto& reference : references) {
        *reference.target = XPLMFindDataRef(reference.name);
        if (!*reference.target) {
            XPLMDebugString("G940 FF: required dataref missing: ");
            XPLMDebugString(reference.name); XPLMDebugString("\n");
            return 0;
        }
    }
    return 1;
}

PLUGIN_API int XPluginEnable() {
    if (!enabled) {
        XPLMRegisterFlightLoopCallback(flightLoopCallback, 0.01f, nullptr);
        enabled = true;
    }
    return 1;
}

PLUGIN_API void XPluginDisable() {
    if (enabled) XPLMUnregisterFlightLoopCallback(flightLoopCallback, nullptr);
    enabled = false;
    if (!g940::closeForceFeedback()) reportError();
    forceReady = false;
    forceSmoother.reset();
}

PLUGIN_API void XPluginStop() { XPluginDisable(); }
PLUGIN_API void XPluginReceiveMessage(XPLMPluginID, int, void *) {}
