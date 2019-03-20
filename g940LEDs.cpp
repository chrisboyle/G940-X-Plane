#include <fcntl.h>
#include <stdio.h>
#include <string>
#include <sstream>
#include <string.h>
#include <unistd.h>
#include "XPLMDataAccess.h"
#include "XPLMProcessing.h"

#ifndef XPLM300
	#error This is made to be compiled against the XPLM300 SDK
#endif

using namespace std;

XPLMDataRef autopilotRef, engTypeRef, gliderRef, carbHeatRef, haveFlapsRef, flapsRef, isRetractRef, gearRef, landLightRef, haveSbrkRef, speedBrakeRef;

float flightLoopCallback(
		float inElapsedSinceLastCall,
		float inElapsedTimeSinceLastFlightLoop,
		int   inCounter,
		void *inRefcon);

PLUGIN_API int XPluginStart(char *outName, char *outSig, char *outDesc) {
	strcpy(outName, "G940 LEDs");
	strcpy(outSig,  "name.boyle.chris.xpg940leds");
	strcpy(outDesc, "Sets G940 LEDs via /sys/class/leds");

	// TODO cache until XPLM_MSG_PLANE_LOADED
	engTypeRef    = XPLMFindDataRef("sim/aircraft/prop/acf_en_type");
	gliderRef     = XPLMFindDataRef("sim/aircraft2/metadata/is_glider");
	haveFlapsRef  = XPLMFindDataRef("sim/aircraft/parts/acf_flapEQ");
	isRetractRef  = XPLMFindDataRef("sim/aircraft/gear/acf_gear_retract");
	haveSbrkRef   = XPLMFindDataRef("sim/aircraft/parts/acf_sbrkEQ");

	autopilotRef  = XPLMFindDataRef("sim/cockpit2/autopilot/autopilot_on_or_cws");
	carbHeatRef   = XPLMFindDataRef("sim/cockpit2/engine/actuators/carb_heat_ratio");
	flapsRef      = XPLMFindDataRef("sim/cockpit2/controls/flap_handle_deploy_ratio");
	gearRef       = XPLMFindDataRef("sim/flightmodel2/gear/deploy_ratio");
	landLightRef  = XPLMFindDataRef("sim/cockpit/electrical/landing_lights_on");
	speedBrakeRef = XPLMFindDataRef("sim/flightmodel2/controls/speedbrake_ratio");

	XPLMRegisterFlightLoopCallback(flightLoopCallback, 0.01, NULL);

	printf("G940 LEDs initialised\n");
	return 1;
}

PLUGIN_API void XPluginDisable(void) {}
PLUGIN_API int  XPluginEnable(void)  { return 1; }
PLUGIN_API void XPluginReceiveMessage(XPLMPluginID inFrom, int inMsg, void * inParam) {}

enum LEDColour { UNKNOWN = -1, OFF = 0, RED = 1, GREEN = 2, AMBER = RED | GREEN };

LEDColour last[8] = {
	UNKNOWN, UNKNOWN, UNKNOWN, UNKNOWN,
	UNKNOWN, UNKNOWN, UNKNOWN, UNKNOWN
};

void writeLEDComponent(const int label, const string& component, bool onOff) {
	ostringstream path;
	path << "/sys/class/leds/g940:" << component << ":P" << label << "/brightness";
	int fd = open(path.str().c_str(), O_RDWR);
	if (fd < 0) {
		return;
	}
	write(fd, onOff ? "1" : "0", 1);
	close(fd);
}

void setLED(const int label, const LEDColour colour)
{
	writeLEDComponent(label, "red",   colour & RED);
	writeLEDComponent(label, "green", colour & GREEN);
}

LEDColour greenOn(bool in) {
	return in ? GREEN : RED;
}

LEDColour green1(float in) {
	return  (in <= 0.0) ? RED :
		(in >= 1.0) ? GREEN :
		AMBER;
}

LEDColour topHalf(float in) {
	return  (in <= 0.125) ? RED :
		(in <= 0.375) ? AMBER :
		GREEN;
}

LEDColour botHalf(float in) {
	return  (in <= 0.625) ? RED :
		(in <= 0.875) ? AMBER :
		GREEN;
}

float flightLoopCallback(
		float  inElapsedSinceLastCall,
		float  inElapsedTimeSinceLastFlightLoop,
		int    inCounter,
		void * inRefcon) {
	bool autopilot = XPLMGetDatai(autopilotRef);
	bool isGlider = XPLMGetDatai(gliderRef);
	bool haveCarb = XPLMGetDatai(engTypeRef) == 0  // "recip carb"
			&& !isGlider;
	bool haveFlaps = XPLMGetDatai(haveFlapsRef);
	bool haveSbrk = XPLMGetDatai(haveSbrkRef);
	int isRetractable = XPLMGetDatai(isRetractRef);
	bool landLight = XPLMGetDatai(landLightRef);
	float carbHeat, flaps, gear, speedBrakes;
	XPLMGetDatavf(carbHeatRef, &carbHeat, 0, 1);
	flaps = XPLMGetDataf(flapsRef);
	XPLMGetDatavf(gearRef,     &gear,     0, 1);
	speedBrakes = XPLMGetDataf(speedBrakeRef);

	const LEDColour wanted[8] = {
		haveSbrk      ? topHalf(speedBrakes) : OFF,
		haveFlaps     ? topHalf(flaps)       : OFF,
		haveCarb      ? green1(carbHeat)     : OFF,
		isGlider ? OFF : greenOn(autopilot),
		haveSbrk      ? botHalf(speedBrakes) : OFF,
		haveFlaps     ? botHalf(flaps)       : OFF,
		isGlider ? OFF : greenOn(landLight),
		isRetractable ? green1(gear)         : OFF,
	};

	for (int i = 0; i < 8; i++) {
		if (last[i] != wanted[i]) {
			setLED(i + 1, wanted[i]);
			last[i] = wanted[i];
		}
	}
	return 0.2;
}

PLUGIN_API void XPluginStop(void) {
	for (int i = 0; i < 8; i++) {
		setLED(i + 1, GREEN);
	}
}
