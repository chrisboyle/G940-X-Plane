// Test-only simulator/backend replacements. Never linked into a plugin.
#include "XPLMDefs.h"
// These definitions emulate the host; they must not be marked dllimport.
#undef XPLM_API
#define XPLM_API
#include "XPLMPlugin.h"
#include "XPLMDataAccess.h"
#include "XPLMProcessing.h"
#include "XPLMUtilities.h"
#include "g940Backend.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>

PLUGIN_API int XPluginStart(char *, char *, char *);
PLUGIN_API int XPluginEnable();
PLUGIN_API void XPluginDisable();
PLUGIN_API void XPluginStop();

namespace {
enum Type { DATA_INTEGER, DATA_FLOAT, DATA_INTEGERS, DATA_FLOATS };
struct Ref { Type type; float value; };
std::map<std::string, Ref> refs;
XPLMFlightLoop_f callback = nullptr;
int registrations = 0, unregistrations = 0;
int opens = 0, closes = 0;
int releases = 0;
bool deviceOpen = false, allowOpen = true, allowUpdate = true;
bool missingRef = false;
g940::LEDState observedLEDs;
g940::ForceState observedForce;
Ref& ref(XPLMDataRef data) { assert(data); return *static_cast<Ref *>(data); }
}

extern "C" {
XPLMDataRef XPLMFindDataRef(const char *name) {
    if (missingRef) return nullptr;
    auto entry = refs.find(name);
    assert(entry != refs.end());
    return &entry->second;
}
int XPLMGetDatai(XPLMDataRef data) { assert(ref(data).type == DATA_INTEGER); return ref(data).value; }
float XPLMGetDataf(XPLMDataRef data) { assert(ref(data).type == DATA_FLOAT); return ref(data).value; }
int XPLMGetDatavi(XPLMDataRef data, int *out, int offset, int count) {
    assert(ref(data).type == DATA_INTEGERS && offset == 0 && count == 1);
    *out = ref(data).value; return 1;
}
int XPLMGetDatavf(XPLMDataRef data, float *out, int offset, int count) {
    assert(ref(data).type == DATA_FLOATS && offset == 0 && count == 1);
    *out = ref(data).value; return 1;
}
void XPLMRegisterFlightLoopCallback(XPLMFlightLoop_f flightLoop, float, void *) {
    assert(!callback); callback = flightLoop; ++registrations;
}
void XPLMUnregisterFlightLoopCallback(XPLMFlightLoop_f flightLoop, void *) {
    assert(callback == flightLoop); callback = nullptr; ++unregistrations;
}
void XPLMDebugString(const char *) {}
}

namespace g940 {
bool prepareForceFeedback() { return true; }
bool openForceFeedback() { ++opens; return deviceOpen = allowOpen; }
bool releaseForceFeedback() { ++releases; return true; }
bool closeForceFeedback() { if (deviceOpen) ++closes; deviceOpen = false; return true; }
bool updateForceFeedback(const ForceState& state) {
    assert(deviceOpen); observedForce = state;
    if (!allowUpdate) deviceOpen = false;
    return allowUpdate;
}
bool openLEDs() { ++opens; return deviceOpen = allowOpen; }
void closeLEDs() { if (deviceOpen) ++closes; deviceOpen = false; }
bool updateLEDs(const LEDState& state) {
    assert(deviceOpen); observedLEDs = state;
    if (!allowUpdate) deviceOpen = false;
    return allowUpdate;
}
const char *backendError() { return "simulated device error"; }
}

int main() {
    char name[256], signature[256], description[256];
#ifdef TEST_LEDS
    refs = {
        {"sim/aircraft/prop/acf_en_type", {DATA_INTEGERS, 1}},
        {"sim/aircraft2/metadata/is_glider", {DATA_INTEGER, 0}},
        {"sim/aircraft/parts/acf_flapEQ", {DATA_INTEGER, 1}},
        {"sim/aircraft/gear/acf_gear_retract", {DATA_INTEGER, 1}},
        {"sim/aircraft/parts/acf_sbrkEQ", {DATA_INTEGER, 1}},
        {"sim/cockpit2/autopilot/autopilot_on_or_cws", {DATA_INTEGER, 1}},
        {"sim/cockpit2/engine/actuators/carb_heat_ratio", {DATA_FLOATS, 1}},
        {"sim/cockpit2/controls/flap_handle_deploy_ratio", {DATA_FLOAT, .25}},
        {"sim/flightmodel2/gear/deploy_ratio", {DATA_FLOATS, .5}},
        {"sim/cockpit/electrical/landing_lights_on", {DATA_INTEGER, 0}},
        {"sim/flightmodel2/controls/speedbrake_ratio", {DATA_FLOAT, .75}}
    };
#else
    refs = {
        {"sim/joystick/yoke_roll_ratio", {DATA_FLOAT, .1}},
        {"sim/joystick/yoke_pitch_ratio", {DATA_FLOAT, .2}},
        {"sim/flightmodel/position/true_airspeed", {DATA_FLOAT, 25.722222}},
        {"sim/aircraft/view/acf_Vne", {DATA_FLOAT, 100}},
        {"sim/flightmodel/position/alpha", {DATA_FLOAT, 5}},
        {"sim/flightmodel2/controls/elevator_trim", {DATA_FLOAT, .1}},
        {"sim/flightmodel2/controls/aileron_trim", {DATA_FLOAT, 0}},
        {"sim/time/paused", {DATA_INTEGER, 0}}
    };
#endif
    missingRef = true;
    assert(XPluginStart(name, signature, description) == 0);
    assert(!callback);
    missingRef = false;
    assert(XPluginStart(name, signature, description) == 1);
    assert(!callback);
    assert(XPluginEnable() == 1 && callback);
    assert(XPluginEnable() == 1 && registrations == 1);
    assert(callback(0, 0, 0, nullptr) > 0);
#ifdef TEST_LEDS
    const g940::LEDState expected = {{g940::GREEN, g940::AMBER, g940::OFF, g940::GREEN,
                                     g940::AMBER, g940::RED, g940::RED, g940::AMBER}};
    assert(observedLEDs == expected); // injected engine must not show carb heat
    refs["sim/aircraft/prop/acf_en_type"].value = 0;
    callback(0, 0, 0, nullptr);
    assert(observedLEDs[2] == g940::GREEN);
#else
    for (int i = 0; i < 100; ++i) callback(.02f, 0, 0, nullptr);
    assert(std::abs(observedForce.speedRatio - .5) < .001);
    assert(std::abs(observedForce.roll) < .001);
    assert(std::abs(observedForce.pitch) < .001);
    assert(std::abs(observedForce.rollForce + .3) < .001);
    assert(std::abs(observedForce.pitchForce + .3) < .001);
    refs["sim/flightmodel/position/true_airspeed"].value = 0;
    refs["sim/flightmodel/position/alpha"].value = -121;
    refs["sim/flightmodel2/controls/elevator_trim"].value = .2;
    for (int i = 0; i < 100; ++i) callback(.02f, 0, 0, nullptr);
    assert(std::abs(observedForce.speedRatio - g940::minimumForceRatio) < .001);
    assert(std::abs(observedForce.pitch) < .001); // no spurious ground AoA pull
    refs["sim/aircraft/view/acf_Vne"].value = 0;
    callback(0, 0, 0, nullptr);
    assert(observedForce.speedRatio == 0);
    refs["sim/time/paused"].value = 1;
    callback(0, 0, 0, nullptr);
    assert(deviceOpen && releases == 1 && closes == 0);
    refs["sim/time/paused"].value = 0;
    callback(0, 0, 0, nullptr);
    assert(deviceOpen);
    refs["sim/aircraft/view/acf_Vne"].value = 100;
    refs["sim/flightmodel/position/true_airspeed"].value = 25.722222;
    refs["sim/flightmodel/position/alpha"].value = 5;
    refs["sim/flightmodel2/controls/elevator_trim"].value = .1;
    for (int i = 0; i < 100; ++i) callback(.02f, 0, 0, nullptr);
    refs["sim/time/paused"].value = 1;
    for (int i = 0; i < 25; ++i) callback(.02f, 0, 0, nullptr);
    assert(deviceOpen && releases == 1 && closes == 0);
    assert(std::abs(observedForce.effectScale - .5) < .001);
    refs["sim/time/paused"].value = 0;
    callback(.02f, 0, 0, nullptr);
    assert(observedForce.effectScale > .5 && observedForce.effectScale < .53);
    refs["sim/time/paused"].value = 1;
    for (int i = 0; i < 60; ++i) callback(.02f, 0, 0, nullptr);
    assert(deviceOpen && releases == 2 && closes == 0);
    const int opensBeforePausedCallback = opens;
    callback(.2f, 0, 0, nullptr);
    assert(opens == opensBeforePausedCallback); // paused callback must not restart live effects
    refs["sim/time/paused"].value = 0;
    refs["sim/flightmodel2/controls/elevator_trim"].value = .3f;
    refs["sim/flightmodel2/controls/aileron_trim"].value = -.1f;
    callback(.02f, 0, 0, nullptr);
    assert(deviceOpen && observedForce.speedRatio <= .020001);
    // Resume establishes the current trim center before raising the force,
    // so neither grip mode briefly pulls toward a stale neutral position.
    assert(std::abs(observedForce.pitch - .3f) < .00001f);
    assert(std::abs(observedForce.roll + .3f) < .00001f);
    assert(observedForce.effectScale <= .020001f);
#endif
    allowUpdate = false;
    assert(callback(0, 0, 0, nullptr) == 5.0f);
    allowOpen = false;
    assert(callback(0, 0, 0, nullptr) == 5.0f);
    allowOpen = allowUpdate = true;
    callback(0, 0, 0, nullptr);
    assert(deviceOpen);
    XPluginDisable();
    assert(!callback && !deviceOpen && unregistrations == 1);
    XPluginDisable();
    assert(unregistrations == 1);
    XPluginEnable();
    callback(0, 0, 0, nullptr);
    XPluginStop();
    assert(!callback && !deviceOpen && registrations == 2 && unregistrations == 2);
    std::puts("Dataref types, reconnect, pause, and plugin lifecycle passed.");
}

// Compile the real plugin against this host's declarations. On Windows this
// also avoids importing SDK symbols from a DLL in the standalone test runner.
#ifdef TEST_LEDS
#include "../g940LEDs.cpp"
#else
#include "../g940FF.cpp"
#endif
