#include <cstring>
#include <string>
#include "XPLMPlugin.h"
#include "XPLMDataAccess.h"
#include "XPLMProcessing.h"
#include "XPLMUtilities.h"
#include "g940Backend.h"

#ifndef XPLM300
#error This plugin requires the XPLM300 API
#endif

namespace {
XPLMDataRef autopilotRef, engTypeRef, gliderRef, carbHeatRef, haveFlapsRef,
    flapsRef, isRetractRef, gearRef, landLightRef, haveSbrkRef, speedBrakeRef;
bool enabled = false;
bool ledsReady = false;

g940::LEDColour greenOn(bool value) { return value ? g940::GREEN : g940::RED; }
g940::LEDColour green1(float value) {
    return value <= 0.0f ? g940::RED : value >= 1.0f ? g940::GREEN : g940::AMBER;
}
g940::LEDColour topHalf(float value) {
    return value <= 0.125f ? g940::RED : value <= 0.375f ? g940::AMBER : g940::GREEN;
}
g940::LEDColour bottomHalf(float value) {
    return value <= 0.625f ? g940::RED : value <= 0.875f ? g940::AMBER : g940::GREEN;
}
void reportError() {
    const std::string message = std::string("G940 LEDs: ") + g940::backendError() + "\n";
    XPLMDebugString(message.c_str());
}

float flightLoopCallback(float, float, int, void *) {
    if (!ledsReady) {
        if (!g940::openLEDs()) { reportError(); return 5.0f; }
        ledsReady = true;
        XPLMDebugString("G940 LEDs: LED device connected\n");
    }
    const bool isGlider = XPLMGetDatai(gliderRef);
    int engineType = -1;
    const bool haveCarb = XPLMGetDatavi(engTypeRef, &engineType, 0, 1) == 1 &&
                          engineType == 0 && !isGlider;
    float carbHeat = 0.0f, gear = 0.0f;
    XPLMGetDatavf(carbHeatRef, &carbHeat, 0, 1);
    XPLMGetDatavf(gearRef, &gear, 0, 1);
    const float flaps = XPLMGetDataf(flapsRef);
    const float speedBrakes = XPLMGetDataf(speedBrakeRef);
    const bool haveFlaps = XPLMGetDatai(haveFlapsRef);
    const bool haveSpeedBrakes = XPLMGetDatai(haveSbrkRef);
    const g940::LEDState wanted = {{
        haveSpeedBrakes ? topHalf(speedBrakes) : g940::OFF,
        haveFlaps ? topHalf(flaps) : g940::OFF,
        haveCarb ? green1(carbHeat) : g940::OFF,
        isGlider ? g940::OFF : greenOn(XPLMGetDatai(autopilotRef)),
        haveSpeedBrakes ? bottomHalf(speedBrakes) : g940::OFF,
        haveFlaps ? bottomHalf(flaps) : g940::OFF,
        isGlider ? g940::OFF : greenOn(XPLMGetDatai(landLightRef)),
        XPLMGetDatai(isRetractRef) ? green1(gear) : g940::OFF
    }};
    if (!g940::updateLEDs(wanted)) {
        reportError(); ledsReady = false; return 5.0f;
    }
    return 0.2f;
}
}

PLUGIN_API int XPluginStart(char *outName, char *outSig, char *outDesc) {
    std::strcpy(outName, "G940 LEDs");
    std::strcpy(outSig, "name.boyle.chris.xpg940leds");
    std::strcpy(outDesc, "Sets the Logitech G940 throttle LEDs from aircraft state.");
    struct Reference { XPLMDataRef *target; const char *name; };
    const Reference references[] = {
        {&engTypeRef, "sim/aircraft/prop/acf_en_type"},
        {&gliderRef, "sim/aircraft2/metadata/is_glider"},
        {&haveFlapsRef, "sim/aircraft/parts/acf_flapEQ"},
        {&isRetractRef, "sim/aircraft/gear/acf_gear_retract"},
        {&haveSbrkRef, "sim/aircraft/parts/acf_sbrkEQ"},
        {&autopilotRef, "sim/cockpit2/autopilot/autopilot_on_or_cws"},
        {&carbHeatRef, "sim/cockpit2/engine/actuators/carb_heat_ratio"},
        {&flapsRef, "sim/cockpit2/controls/flap_handle_deploy_ratio"},
        {&gearRef, "sim/flightmodel2/gear/deploy_ratio"},
        {&landLightRef, "sim/cockpit/electrical/landing_lights_on"},
        {&speedBrakeRef, "sim/flightmodel2/controls/speedbrake_ratio"}
    };
    for (const auto& reference : references) {
        *reference.target = XPLMFindDataRef(reference.name);
        if (!*reference.target) {
            XPLMDebugString("G940 LEDs: required dataref missing: ");
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
    g940::closeLEDs();
    ledsReady = false;
}

PLUGIN_API void XPluginStop() { XPluginDisable(); }
PLUGIN_API void XPluginReceiveMessage(XPLMPluginID, int, void *) {}
