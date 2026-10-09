// Exercise the real native backend without USB access or motor movement.
#include "g940Backend.h"
#include "g940HID.h"
#include <cassert>
#include <cstdio>
#include <vector>

namespace {
bool failRead = false, failOutput = false;
int failFeatureID = 0, mismatchFeatureID = 0, corruptFeatureID = 0;
int connections = 0;
std::vector<std::vector<uint8_t>> features, outputs;
const std::array<std::array<uint8_t, 4>, 2> originalIdle = {{{{5, 20, 60, 127}}, {{6, 26, 100, 90}}}};
auto idle = originalIdle;
const std::array<uint8_t, 5> originalCenters = {{10, 0x34, 0x12, 0xcb, 0xed}};
auto centers = originalCenters;
}

namespace g940 {
HIDDevice::~HIDDevice() { close(); }
bool HIDDevice::open() {
    if (!isOpen()) { handle_ = this; ++connections; }
    return true;
}
void HIDDevice::close() { if (isOpen()) --connections; handle_ = nullptr; }
bool HIDDevice::getReport(Report type, std::span<uint8_t> report) {
    assert(type == Report::Feature);
    const size_t length = report.size();
    assert(isOpen());
    if (failRead) { error_ = "simulated feature read failure"; return false; }
    if (report[0] == 3) {
        assert(length == 3); report[1] = 0; report[2] = 0xff;
    } else if (report[0] == 10) {
        assert(length == 5);
        std::copy(centers.begin(), centers.end(), report.begin());
        if (corruptFeatureID == 10) { report[0] = 4; corruptFeatureID = 0; }
    } else {
        assert(length == 4 && (report[0] == 5 || report[0] == 6));
        const int id = report[0];
        std::copy(idle[id - 5].begin(), idle[id - 5].end(), report.begin());
        if (id == mismatchFeatureID && report[1] == 0) {
            report[1] = 1; mismatchFeatureID = 0;
        }
        if (id == corruptFeatureID) { report[0] = 4; corruptFeatureID = 0; }
    }
    return true;
}
bool HIDDevice::setReport(Report type, std::span<const uint8_t> report) {
    assert(isOpen());
    if (type == Report::Output) {
        outputs.emplace_back(report.begin(), report.end());
        if (failOutput) { failOutput = false; error_ = "simulated output failure"; return false; }
        return true;
    }
    assert(type == Report::Feature);
    const size_t length = report.size();
    features.emplace_back(report.begin(), report.end());
    if (report[0] == 5 || report[0] == 6) {
        assert(length == 4);
        std::copy(report.begin(), report.end(), idle[report[0] - 5].begin());
        // A failed transfer can leave the device modified; rollback must cover both axes.
        if (report[0] == failFeatureID) {
            failFeatureID = 0; error_ = "simulated feature write failure"; return false;
        }
    } else if (report[0] == 10) {
        assert(length == 5); std::copy(report.begin(), report.end(), centers.begin());
        if (failFeatureID == 10) {
            failFeatureID = 0; error_ = "simulated feature write failure"; return false;
        }
    } else assert(report[0] == 3 && length == 3);
    return true;
}
}

int main() {
    using namespace g940;
    const std::array<std::array<uint8_t, 4>, 2> disabledIdle = {{{{5, 0, 0, 0}}, {{6, 0, 0, 0}}}};
    assert(prepareForceFeedback() && connections == 1); // also works before a paused flight
    assert(idle == disabledIdle && features.size() == 2);
    assert(openForceFeedback() && connections == 1 && features.size() == 2);
    const auto stop = stopReport();
    assert(outputs.back() == std::vector<uint8_t>(stop.begin(), stop.end()));
    assert(updateForceFeedback({.2, -.3, .5}));
    assert(outputs.back()[7] != 0);
    const auto expected = forceReport({.2, -.3, .5});
    assert(idle[0] == idleForceReport(expected, 0) && idle[1] == idleForceReport(expected, 1));
    assert(centers == idleCenterReport(expected));
    const auto featureWrites = features.size();
    assert(updateForceFeedback({.2, -.3, .5}) && features.size() == featureWrites);
    assert(updateForceFeedback({.2, -.3, .5, 0, 0, 0}));
    assert(outputs.back() == std::vector<uint8_t>(stop.begin(), stop.end()));
    assert(releaseForceFeedback() && connections == 1 && idle == disabledIdle);
    assert(openForceFeedback() && idle == disabledIdle); // pause must preserve the original backup
    failOutput = true;
    assert(!updateForceFeedback({.2, -.3, .5}));
    assert(connections == 0);
    assert(idle == originalIdle && centers == originalCenters);
    assert(outputs.back() == std::vector<uint8_t>(stop.begin(), stop.end()));
    assert(std::string(backendError()) == "simulated output failure");

    failRead = true;
    const auto writesBeforeReadFailure = features.size();
    assert(!prepareForceFeedback() && connections == 0 && idle == originalIdle);
    assert(features.size() == writesBeforeReadFailure); // no writes without a complete backup
    failRead = false;
    corruptFeatureID = 6;
    assert(!prepareForceFeedback() && connections == 0 && idle == originalIdle);
    assert(features.size() == writesBeforeReadFailure); // never write an unexpected report ID
    corruptFeatureID = 10;
    assert(!prepareForceFeedback() && connections == 0 && centers == originalCenters);
    assert(features.size() == writesBeforeReadFailure);
    failFeatureID = 6;
    assert(!openForceFeedback() && connections == 0 && idle == originalIdle);
    assert(std::string(backendError()) == "simulated feature write failure");
    mismatchFeatureID = 6;
    assert(!openForceFeedback() && connections == 0 && idle == originalIdle);
    assert(std::string(backendError()).find("read back") != std::string::npos);
    assert(openForceFeedback());
    failFeatureID = 10;
    assert(!updateForceFeedback({.4, -.4, .5}) && connections == 0);
    assert(idle == originalIdle && centers == originalCenters);
    assert(openForceFeedback() && idle == disabledIdle);
    failFeatureID = 5;
    assert(!closeForceFeedback() && connections == 0 && idle == originalIdle && centers == originalCenters);
    assert(features.back()[0] == 6); // still restore the other axis after a failure
    assert(openForceFeedback() && idle == disabledIdle);
    assert(closeForceFeedback() && connections == 0 && idle == originalIdle && centers == originalCenters);

    features.clear();
    failRead = true;
    assert(!openLEDs() && connections == 0);
    failRead = false;
    assert(openLEDs() && connections == 1);
    LEDState colours;
    colours.fill(RED);
    assert(updateLEDs(colours));
    assert(updateLEDs(colours)); // unchanged state still checks the USB device
    assert(features.size() == 2);
    closeLEDs();
    assert(connections == 0);
    assert((features.back() == std::vector<uint8_t>{3, 0, 0xff}));
    std::puts("Native HID idle backup, pause retention, rollback, restore, and LED cleanup passed.");
}
