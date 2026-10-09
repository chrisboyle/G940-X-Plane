#include "g940HID.h"
#include "g940Protocol.h"
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {
volatile std::sig_atomic_t interrupted = 0;
void onSignal(int) { interrupted = 1; }
using Clock = std::chrono::steady_clock;
enum Mode { READ, LED, SPRING, ROLL, PITCH };

void usage(const char *program) {
    std::printf("Usage: %s [--led-test | --force-test | --roll-test | --pitch-test]\n"
                "       [--seconds 1..30] [--reverse] [--magnitude 1..16384]\n"
                "No arguments: read LED state and grip sensor only.\n"
                "--force-test: centered spring on both axes at 10%% saturation.\n"
                "--roll-test / --pitch-test: constant force, default 4000/32767.\n"
                "--magnitude: constant-force level, limited to half the nominal range.\n"
                "--reverse: reverse a constant-force test.\n"
                "Force tests include a zero-force stage of the same duration.\n", program);
}

bool prepare() {
    std::puts("Keep X-Plane closed during this comparison. Press Enter when ready.");
    std::fflush(stdout);
    int c;
    do { c = std::getchar(); } while (c != '\n' && c != EOF && !interrupted);
    return c == '\n' && !interrupted;
}

class StopForce {
public:
    explicit StopForce(g940::HIDDevice& device) : device_(device), stopped_(false) {}
    ~StopForce() { if (!stopped_) stop(); }
    bool stop() {
        const auto report = g940::stopReport();
        stopped_ = device_.setOutput(report);
        if (!stopped_) std::fprintf(stderr, "Stop force failed: %s\n", device_.error().c_str());
        return stopped_;
    }
private:
    g940::HIDDevice& device_;
    bool stopped_;
};

bool waitForGrip(g940::HIDDevice& device) {
    std::puts("Armed. Cover the grip sensor and hold it throughout both stages.");
    std::fflush(stdout);
    auto heldSince = Clock::now();
    const auto deadline = heldSince + std::chrono::seconds(45);
    while (!interrupted && Clock::now() < deadline) {
        bool covered = false;
        if (!device.readGrip(covered)) {
            std::fprintf(stderr, "Read grip sensor failed: %s\n", device.error().c_str());
            return false;
        }
        if (!covered) heldSince = Clock::now();
        else if (Clock::now() - heldSince >= std::chrono::seconds(2)) {
            std::puts("Grip sensor: HAND ON, held for two seconds.");
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::fprintf(stderr, interrupted ? "Comparison interrupted.\n" : "Timed out waiting for grip sensor.\n");
    return false;
}

bool forceStage(g940::HIDDevice& device, const std::array<uint8_t, 64>& report,
                const char *label, int seconds) {
    std::printf("%s for %d seconds.\n", label, seconds);
    std::fflush(stdout);
    const auto end = Clock::now() + std::chrono::seconds(seconds);
    while (!interrupted && Clock::now() < end) {
        bool covered = false;
        if (!device.readGrip(covered)) {
            std::fprintf(stderr, "%s\n", device.error().c_str()); return false;
        }
        if (!covered) {
            std::fprintf(stderr, "Grip released: comparison stopped.\n"); return false;
        }
        if (!device.setOutput(report)) {
            std::fprintf(stderr, "%s\n", device.error().c_str()); return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return !interrupted;
}
}

int main(int argc, char **argv) {
    Mode mode = READ;
    int seconds = 3;
    int magnitude = 4000;
    bool magnitudeSet = false;
    bool reverse = false;
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (!std::strcmp(arg, "--help")) { usage(argv[0]); return 0; }
        if ((!std::strcmp(arg, "--seconds") || !std::strcmp(arg, "--magnitude")) && i + 1 < argc) {
            const bool duration = !std::strcmp(arg, "--seconds");
            char *end = nullptr;
            const long value = std::strtol(argv[++i], &end, 10);
            if (!*argv[i] || *end || value < 1 || value > (duration ? 30 : 16384)) {
                usage(argv[0]); return 2;
            }
            if (duration) seconds = static_cast<int>(value);
            else { magnitude = static_cast<int>(value); magnitudeSet = true; }
        } else if (!std::strcmp(arg, "--reverse")) {
            reverse = true;
        } else {
            Mode selected;
            if (!std::strcmp(arg, "--led-test")) selected = LED;
            else if (!std::strcmp(arg, "--force-test")) selected = SPRING;
            else if (!std::strcmp(arg, "--roll-test")) selected = ROLL;
            else if (!std::strcmp(arg, "--pitch-test")) selected = PITCH;
            else { usage(argv[0]); return 2; }
            if (mode != READ) { usage(argv[0]); return 2; }
            mode = selected;
        }
    }
    if ((reverse || magnitudeSet) && mode != ROLL && mode != PITCH) { usage(argv[0]); return 2; }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    g940::HIDDevice device;
    if (!device.open()) { std::fprintf(stderr, "%s\n", device.error().c_str()); return 1; }
    std::puts("Connected to Logitech G940 (046d:c287), without exclusive access.");
    std::array<uint8_t, 3> original = {{3}};
    if (!device.getFeature(original)) {
        std::fprintf(stderr, "%s\n", device.error().c_str()); return 1;
    }
    std::printf("LED report: ID=%02x red=%02x green=%02x\n", original[0], original[1], original[2]);
    if (mode == READ) {
        bool covered = false;
        if (!device.readGrip(covered)) { std::fprintf(stderr, "%s\n", device.error().c_str()); return 1; }
        std::printf("Grip sensor: %s\n", covered ? "HAND ON" : "HAND OFF");
        return 0;
    }
    if (mode == LED) {
        std::printf("Next: red/green/amber/off LED pattern for %d seconds.\n", seconds);
        if (!prepare()) return 1;
        const g940::LEDState colours = {{g940::RED, g940::GREEN, g940::AMBER, g940::OFF,
                                        g940::RED, g940::GREEN, g940::AMBER, g940::OFF}};
        const auto report = g940::ledReport(colours);
        const bool applied = device.setFeature(report);
        if (applied) {
            const auto end = Clock::now() + std::chrono::seconds(seconds);
            while (!interrupted && Clock::now() < end)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        std::array<uint8_t, 3> observed = {{3}};
        const bool readBack = applied && device.getFeature(observed);
        const bool restored = device.setFeature(original);
        if (!readBack || observed != report || !restored || interrupted) {
            std::fprintf(stderr, "LED comparison/restore failed or interrupted: %s\n", device.error().c_str());
            return 1;
        }
        std::puts("LED report readback matches; original LED state restored.");
        return 0;
    }
    auto report = mode == SPRING ? g940::forceReport({0.0, 0.0, 0.1}) : g940::stopReport();
    // Keep the advertised 10% of each configured axis cap independent of
    // the flight model's airspeed gains and stationary baseline.
    if (mode == SPRING) {
        for (unsigned axis = 0; axis < 2; ++axis)
            g940::put16(report.data() + 1 + 30 * axis + 12, 0.1 * g940::springMaximums[axis]);
    }
    if (mode != SPRING) g940::put16(report.data() + (mode == ROLL ? 1 : 31), reverse ? -magnitude : magnitude);
    std::printf("Next: %s%s for %d seconds, then zero force for %d seconds.\n"
                "The G940 motor power adapter must be connected.\n",
                mode == SPRING ? "centered spring at 10% saturation" :
                mode == ROLL ? "roll-axis constant force" : "pitch-axis constant force",
                reverse ? " (reverse direction)" : "", seconds, seconds);
    if (mode != SPRING)
        std::printf("Constant-force magnitude: %d/32767 (%.1f%% of nominal range).\n",
                    magnitude, 100.0 * magnitude / 32767.0);
    if (!prepare()) return 1;
    StopForce cleanup(device);
    const auto stop = g940::stopReport();
    if (!device.setOutput(stop) || !waitForGrip(device)) return 1;
    const bool completed = forceStage(device, report, "FORCE ON", seconds) &&
                           forceStage(device, stop, "FORCE OFF", seconds);
    const bool stopped = cleanup.stop();
    if (!completed || !stopped) return 1;
    std::puts("Comparison complete; zero-force report sent. Confirm the change in feel physically.");
}
