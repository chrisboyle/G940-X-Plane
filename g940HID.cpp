#include "g940HID.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>

#if APL
#include <IOKit/hid/IOHIDManager.h>
#elif IBM
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#endif

namespace g940 {
namespace {
#if APL || IBM
const unsigned vendorID = 0x046d;
const unsigned productID = 0xc287;
std::string deviceError(const char *operation, unsigned code) {
    char message[160];
    std::snprintf(message, sizeof(message), "%s failed (0x%08x)", operation, code);
    return message;
}
#endif
}

HIDDevice::~HIDDevice() { close(); }

bool HIDDevice::open() {
    if (isOpen()) return true;
    error_ = "Logitech G940 (046d:c287) not found";
#if APL
    IOHIDManagerRef manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!manager) { error_ = "Cannot create HID manager"; return false; }
    int vendor = vendorID, product = productID;
    CFNumberRef vendorNumber = CFNumberCreate(nullptr, kCFNumberIntType, &vendor);
    CFNumberRef productNumber = CFNumberCreate(nullptr, kCFNumberIntType, &product);
    const void *keys[] = {CFSTR(kIOHIDVendorIDKey), CFSTR(kIOHIDProductIDKey)};
    const void *values[] = {vendorNumber, productNumber};
    CFDictionaryRef match = CFDictionaryCreate(nullptr, keys, values, 2,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    IOHIDManagerSetDeviceMatching(manager, match);
    CFSetRef devices = IOHIDManagerCopyDevices(manager);
    if (devices) {
        std::vector<const void *> list(CFSetGetCount(devices));
        CFSetGetValues(devices, list.data());
        for (const void *entry : list) {
            IOHIDDeviceRef device = static_cast<IOHIDDeviceRef>(const_cast<void *>(entry));
            const IOReturn result = IOHIDDeviceOpen(device, kIOHIDOptionsTypeNone);
            if (result == kIOReturnSuccess) {
                handle_ = const_cast<void *>(CFRetain(device));
                break;
            }
            error_ = deviceError("IOHIDDeviceOpen", result);
        }
        CFRelease(devices);
    }
    CFRelease(match); CFRelease(vendorNumber); CFRelease(productNumber);
    CFRelease(manager);
#elif IBM
    GUID hidGUID;
    HidD_GetHidGuid(&hidGUID);
    HDEVINFO devices = SetupDiGetClassDevsW(&hidGUID, nullptr, nullptr,
                                         DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) {
        error_ = deviceError("SetupDiGetClassDevs", GetLastError());
        return false;
    }
    SP_DEVICE_INTERFACE_DATA info = {};
    info.cbSize = sizeof(info);
    for (DWORD index = 0; SetupDiEnumDeviceInterfaces(devices, nullptr, &hidGUID, index, &info); ++index) {
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetailW(devices, &info, nullptr, 0, &size, nullptr);
        if (size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
        std::vector<uint8_t> storage(size);
        auto detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(storage.data());
        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetailW(devices, &info, detail, size, nullptr, nullptr)) continue;
        // Inspect collections without taking input access from X-Plane.
        HANDLE device = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
        if (device == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attributes = {};
        attributes.Size = sizeof(attributes);
        PHIDP_PREPARSED_DATA parsed = nullptr;
        HIDP_CAPS caps = {};
        if (HidD_GetAttributes(device, &attributes) && attributes.VendorID == vendorID &&
            attributes.ProductID == productID && HidD_GetPreparsedData(device, &parsed)) {
            const NTSTATUS result = HidP_GetCaps(parsed, &caps);
            HidD_FreePreparsedData(parsed);
            if (result == HIDP_STATUS_SUCCESS && caps.OutputReportByteLength >= 64 &&
                caps.FeatureReportByteLength >= 5) {
                CloseHandle(device);
                device = CreateFileW(detail->DevicePath, GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_OVERLAPPED, nullptr);
                if (device == INVALID_HANDLE_VALUE) {
                    error_ = deviceError("Open G940 for output", GetLastError());
                    continue;
                }
                HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                if (event) {
                    handle_ = device;
                    writeEvent_ = event;
                    reportLengths_ = {caps.InputReportByteLength, caps.OutputReportByteLength,
                                      caps.FeatureReportByteLength};
                    break;
                }
                error_ = deviceError("Create output event", GetLastError());
            }
        }
        CloseHandle(device);
    }
    SetupDiDestroyDeviceInfoList(devices);
#else
    error_ = "Direct HID is only used on macOS and Windows; Linux uses evdev/sysfs";
#endif
    if (isOpen()) error_.clear();
    return isOpen();
}

void HIDDevice::close() {
    if (!isOpen()) return;
#if APL
    auto device = static_cast<IOHIDDeviceRef>(handle_);
    IOHIDDeviceClose(device, kIOHIDOptionsTypeNone);
    CFRelease(device);
#elif IBM
    CloseHandle(static_cast<HANDLE>(handle_));
    CloseHandle(static_cast<HANDLE>(writeEvent_));
    writeEvent_ = nullptr;
#endif
    handle_ = nullptr;
}

bool HIDDevice::setReport(Report type, std::span<const uint8_t> report) {
    if (!isOpen() || report.empty()) { error_ = "HID device is not open"; return false; }
    const char *operation = type == Report::Feature ? "Set feature report" : "Set output report";
#if APL
    const auto kind = type == Report::Feature ? kIOHIDReportTypeFeature : kIOHIDReportTypeOutput;
    const IOReturn result = IOHIDDeviceSetReport(static_cast<IOHIDDeviceRef>(handle_),
        kind, report[0], report.data(), report.size());
    if (result == kIOReturnSuccess) return true;
    error_ = deviceError(operation, result);
#elif IBM
    const size_t length = reportLengths_[static_cast<unsigned>(type)];
    if (report.size() > length) { error_ = "HID report is too large"; return false; }
    std::vector<uint8_t> padded(length, 0);
    std::copy(report.begin(), report.end(), padded.begin());
    HANDLE device = static_cast<HANDLE>(handle_);
    if (type == Report::Feature) {
        if (HidD_SetFeature(device, padded.data(), padded.size())) return true;
        error_ = deviceError(operation, GetLastError());
        return false;
    }
    // Firmware 1.42 needs interrupt OUT. Keep the buffer/OVERLAPPED alive
    // until completion, including cancellation after the 100 ms timeout.
    OVERLAPPED write = {};
    write.hEvent = static_cast<HANDLE>(writeEvent_);
    ResetEvent(write.hEvent);
    DWORD written = 0;
    BOOL result = WriteFile(device, padded.data(), static_cast<DWORD>(padded.size()),
                            &written, &write);
    if (!result && GetLastError() == ERROR_IO_PENDING) {
        const DWORD wait = WaitForSingleObject(write.hEvent, 100);
        if (wait != WAIT_OBJECT_0) {
            const DWORD code = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
            CancelIoEx(device, &write);
            GetOverlappedResult(device, &write, &written, TRUE);
            error_ = deviceError("Wait for output report", code);
            return false;
        }
        result = GetOverlappedResult(device, &write, &written, FALSE);
    }
    if (result && written == padded.size()) return true;
    error_ = result ? "Incomplete output report" : deviceError(operation, GetLastError());
#else
    error_ = std::string(operation) + " requires the native macOS or Windows backend";
#endif
    return false;
}

bool HIDDevice::getReport(Report type, std::span<uint8_t> report) {
    if (!isOpen() || report.empty()) { error_ = "HID device is not open"; return false; }
    const char *operation = type == Report::Feature ? "Get feature report" : "Read grip sensor";
#if APL
    const auto kind = type == Report::Feature ? kIOHIDReportTypeFeature : kIOHIDReportTypeInput;
    CFIndex actual = report.size();
    const IOReturn result = IOHIDDeviceGetReport(static_cast<IOHIDDeviceRef>(handle_),
        kind, report[0], report.data(), &actual);
    if (result == kIOReturnSuccess && actual == static_cast<CFIndex>(report.size())) return true;
    error_ = result == kIOReturnSuccess ? "Incomplete HID report" : deviceError(operation, result);
#elif IBM
    const size_t length = reportLengths_[static_cast<unsigned>(type)];
    if (report.size() > length) { error_ = "HID report is too large"; return false; }
    std::vector<uint8_t> padded(length, 0);
    padded[0] = report[0];
    HANDLE device = static_cast<HANDLE>(handle_);
    const bool success = type == Report::Feature
        ? HidD_GetFeature(device, padded.data(), padded.size())
        : HidD_GetInputReport(device, padded.data(), padded.size());
    if (success) { std::copy_n(padded.begin(), report.size(), report.begin()); return true; }
    error_ = deviceError(operation, GetLastError());
#else
    error_ = std::string(operation) + " requires the native macOS or Windows backend";
#endif
    return false;
}

bool HIDDevice::readGrip(bool& covered) {
    std::array<uint8_t, 21> report = {1};
    if (!getReport(Report::Input, report)) return false;
    if (report[0] != 1) { error_ = "Invalid grip sensor report"; return false; }
    // Report 1 has 20 payload bytes. Its vendor-defined grip bit is 157.
    covered = (report[20] & 0x20) != 0;
    return true;
}
}
