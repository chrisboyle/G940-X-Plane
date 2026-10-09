#ifndef G940_BACKEND_H
#define G940_BACKEND_H
#include "g940Protocol.h"
namespace g940 {
// Acquire native idle-centering settings even when starting a paused flight.
bool prepareForceFeedback();
bool openForceFeedback();
bool updateForceFeedback(const ForceState& state);
// Stop live effects on pause while retaining native idle-centering ownership.
bool releaseForceFeedback();
// End the session and restore the native device's original idle settings.
bool closeForceFeedback();
bool openLEDs();
bool updateLEDs(const LEDState& state);
void closeLEDs();
const char *backendError();
}
#endif
