#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <unistd.h>
#include <linux/input.h>
#include "XPLMDataAccess.h"
#include "XPLMProcessing.h"

#ifndef XPLM300
	#error This is made to be compiled against the XPLM300 SDK
#endif

using namespace std;

XPLMDataRef rollRef      = NULL;
XPLMDataRef pitchRef     = NULL;
XPLMDataRef speedRef     = NULL;
XPLMDataRef vneRef       = NULL;
XPLMDataRef alphaRef     = NULL;
XPLMDataRef eTrimRef     = NULL;
XPLMDataRef aTrimRef     = NULL;
XPLMDataRef gRef         = NULL;
XPLMDataRef pausedRef    = NULL;
XPLMDataRef stallWarnRef = NULL;

const double RADIANS_TO_FF_DIRECTION = 0x8000 / M_PI;
const double KNOTS_TO_M_S = 0.51444444;

int fd = -1;
bool haveSpring = true;
struct ff_effect effect;
struct input_event play, stop;

#define ERROR_WAIT_SECS 5.0

#define maxmag1(x) min(max(x, -1.0), 1.0)

float flightLoopCallback(
		float inElapsedSinceLastCall,
		float inElapsedTimeSinceLastFlightLoop,
		int   inCounter,
		void *inRefcon);

PLUGIN_API int XPluginStart(char *outName, char *outSig, char *outDesc)
{
	strcpy(outName, "cmb Force Feedback");
	strcpy(outSig,  "name.boyle.chris.xpff");
	strcpy(outDesc, "Connects X-Plane to Linux Force Feedback API.");

	// TODO cache until XPLM_MSG_PLANE_LOADED
	stallWarnRef = XPLMFindDataRef("sim/aircraft/overflow/acf_stall_warn_alpha");
	vneRef       = XPLMFindDataRef("sim/aircraft/view/acf_Vne");

	rollRef      = XPLMFindDataRef("sim/joystick/yoke_roll_ratio");
	pitchRef     = XPLMFindDataRef("sim/joystick/yoke_pitch_ratio");
	speedRef     = XPLMFindDataRef("sim/flightmodel/position/true_airspeed");
	alphaRef     = XPLMFindDataRef("sim/flightmodel/position/alpha");
	eTrimRef     = XPLMFindDataRef("sim/flightmodel2/controls/elevator_trim");
	aTrimRef     = XPLMFindDataRef("sim/flightmodel2/controls/aileron_trim");

	// TODO increase force if:
	//servosRef  = XPLMFindDataRef("sim/cockpit2/autopilot/servos_on");

	// TODO gear rumble:
	// sim/flightmodel2/gear/on_noisy[0]
	// sim/flightmodel/position/groundspeed

	XPLMRegisterFlightLoopCallback(flightLoopCallback, 0.01, NULL);

	printf("Force feedback initialised\n");
	return 1;
}

PLUGIN_API void XPluginStop(void)
{
	if (fd > -1) {
		write(fd, (const void*) &stop, sizeof(stop));
		close(fd);
		fd = -1;
	}
}

PLUGIN_API void XPluginDisable(void) {}
PLUGIN_API int  XPluginEnable(void)  { return 1; }
PLUGIN_API void XPluginReceiveMessage(XPLMPluginID inFrom, int inMsg, void * inParam) {}

void openJoystick(const string& path)
{
	fd = open(path.c_str(), O_RDWR);
	if (fd < 0) {
		return;
	}

	haveSpring = true;
	memset(&effect, 0, sizeof(effect));
	effect.id = -1;
	effect.direction = 0;
	effect.type = FF_SPRING;
	if (ioctl(fd, EVIOCSFF, &effect) == -1) {
		haveSpring = false;
		effect.type = FF_CONSTANT;
		effect.u.constant.level = 0;
		if (ioctl(fd, EVIOCSFF, &effect) == -1) {
			perror((path + " ioctl").c_str());
			close(fd);
			fd = -1;
			return;
		}
	}

	memset(&play, 0, sizeof(play));
	play.type = EV_FF;
	play.code = effect.id;
	play.value = 1;

	memset(&stop, 0, sizeof(stop));
	stop.type = EV_FF;
	stop.code = effect.id;
	stop.value = 0;

	if (write(fd, (const void*) &play, sizeof(play)) == -1) {
		perror("Play effect");
		close(fd);
		fd = -1;
		return;
	}
}

void findJoystick()
{
	DIR *dir;
	struct dirent *ent;
	string inputPath = "/dev/input/";

	if ((dir = opendir(inputPath.c_str())) == NULL) {
		perror((string("open ") + inputPath).c_str());
		return;
	}
	while (fd < 0 && (ent = readdir(dir)) != NULL) {
		openJoystick(inputPath + ent->d_name);
	}
	closedir(dir);
}

float flightLoopCallback(
		float  inElapsedSinceLastCall,
		float  inElapsedTimeSinceLastFlightLoop,
		int    inCounter,
		void * inRefcon)
{
	if (fd < 0) {
		findJoystick();
		if (fd < 0) {
			perror("Can't open joystick");
			return ERROR_WAIT_SECS;
		}
	}

	double pitch     = XPLMGetDataf(pitchRef);
	double roll      = XPLMGetDataf(rollRef);
	double speed     = XPLMGetDataf(speedRef);
	double vne       = XPLMGetDataf(vneRef) * KNOTS_TO_M_S;
	double alpha     = XPLMGetDataf(alphaRef);
	double eTrim     = XPLMGetDataf(eTrimRef);
	double aTrim     = XPLMGetDataf(aTrimRef);

	double pitchForce = (-pitch - (alpha / 50) + eTrim) * 1.5;
	double rollForce = (-roll + aTrim) * 3.0;
	double speedVne = min(speed / vne, 1.0);

	if (haveSpring) {
		double  xSat = 0x8000 * speedVne,
			ySat = 0xffff * speedVne;
		effect.u.condition[0].center = maxmag1(rollForce) * 0x7fff;
		effect.u.condition[0].left_coeff       = 0x4000;
		effect.u.condition[0].left_saturation  = xSat;
		effect.u.condition[0].right_coeff      = 0x4000;
		effect.u.condition[0].right_saturation = xSat;
		effect.u.condition[1].center = maxmag1(pitchForce) * 0x7fff;
		effect.u.condition[1].left_coeff       = 0x4000;
		effect.u.condition[1].left_saturation  = ySat;
		effect.u.condition[1].right_coeff      = 0x4000;
		effect.u.condition[1].right_saturation = ySat;
	} else {
		double totalForce = maxmag1(hypot(-rollForce, pitchForce) * speedVne);
		effect.u.constant.level = totalForce * 0x7fff;
		double directionRadians = atan2(-rollForce, pitchForce);
		effect.direction = directionRadians * RADIANS_TO_FF_DIRECTION;
	}

	if (ioctl(fd, EVIOCSFF, &effect) == -1) {
		perror("Updating effect");
		close(fd);
		fd = -1;
	}

	return 0.02;
}

