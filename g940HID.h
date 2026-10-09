#ifndef G940_HID_H
#define G940_HID_H
#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace g940 {
// Owns a non-exclusive connection so X-Plane can continue reading controls.
class HIDDevice {
    enum class Report { Input, Output, Feature };
public:
    HIDDevice() = default;
    ~HIDDevice();
    bool open();
    void close();
    bool isOpen() const { return handle_ != nullptr; }
    bool setFeature(std::span<const uint8_t> report) { return setReport(Report::Feature, report); }
    bool getFeature(std::span<uint8_t> report) { return getReport(Report::Feature, report); }
    bool setOutput(std::span<const uint8_t> report) { return setReport(Report::Output, report); }
    bool readGrip(bool& covered);
    const std::string& error() const { return error_; }
private:
    HIDDevice(const HIDDevice&) = delete;
    HIDDevice& operator=(const HIDDevice&) = delete;
    bool setReport(Report type, std::span<const uint8_t> report);
    bool getReport(Report type, std::span<uint8_t> report);
    void *handle_ = nullptr;
#if IBM
    std::array<std::size_t, 3> reportLengths_ = {};
    void *writeEvent_ = nullptr;
#endif
    std::string error_;
};
}
#endif
