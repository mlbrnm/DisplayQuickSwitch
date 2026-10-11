#include "windows_scaling.h"
#include <windows.h>
#ifdef QS_TEST_DPI
#include "windows_scaling_test_api.h"
#define GetDisplayConfigBufferSizes qs::testDpi::bufferSizes
#define QueryDisplayConfig qs::testDpi::query
#define DisplayConfigGetDeviceInfo qs::testDpi::getDevice
#define DisplayConfigSetDeviceInfo qs::testDpi::setDevice
#define RegGetValueW qs::testDpi::registry
#endif

namespace qs {
namespace {
// Undocumented CCD packets used by Windows' per-monitor Scale selector. Fail closed on
// errors or unfamiliar ranges. Never fall back to registry writes, global SPI changes,
// or a sign-out. DPI is attached to a SOURCE, not the CCD target/NVIDIA display ID.
struct GetScalePacket { DISPLAYCONFIG_DEVICE_INFO_HEADER header; int32_t minimum, current, maximum; };
struct SetScalePacket { DISPLAYCONFIG_DEVICE_INFO_HEADER header; int32_t relative; };
static_assert(sizeof(GetScalePacket) == 32);
static_assert(sizeof(SetScalePacket) == 24);
constexpr auto GetScaleType = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(-3);
constexpr auto SetScaleType = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(-4);
void checkWindows(LONG status, const char* operation) {
    if (status != ERROR_SUCCESS) throw std::runtime_error(std::string(operation) + " (Windows error " + std::to_string(status) + ").");
}
void rejectCustomScaling() {
    DWORD value = 0, size = sizeof(value);
    const auto status = RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"Win8DpiScaling", RRF_RT_REG_DWORD, nullptr, &value, &size);
    if (status != ERROR_FILE_NOT_FOUND && status != ERROR_PATH_NOT_FOUND) checkWindows(status, "Check Windows custom scaling");
    if (status == ERROR_SUCCESS && value != 0)
        throw std::runtime_error("Windows custom scaling is enabled. Turn it off in Windows Settings before using per-monitor scaling presets.");
}
GetScalePacket readScale(const std::wstring& key) {
    rejectCustomScaling();
    UINT32 pathCount = 0, modeCount = 0;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    LONG status = ERROR_INSUFFICIENT_BUFFER;
    for (int i = 0; i < 5 && status == ERROR_INSUFFICIENT_BUFFER; ++i) {
        checkWindows(GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount), "Count displays for Windows scaling");
        paths.resize(pathCount); modes.resize(modeCount);
        status = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
    }
    checkWindows(status, "Resolve displays for Windows scaling");
    paths.resize(pathCount);
    for (const auto& p : paths) {
        DISPLAYCONFIG_TARGET_DEVICE_NAME name{};
        name.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof(name), p.targetInfo.adapterId, p.targetInfo.id};
        checkWindows(DisplayConfigGetDeviceInfo(&name.header), "Read monitor identity for Windows scaling");
        if (_wcsicmp(name.monitorDevicePath, key.c_str()) != 0) continue;
        // Source scaling affects every target of a cloned source; refuse to change those.
        for (const auto& other : paths)
            if (&other != &p && other.sourceInfo.id == p.sourceInfo.id &&
                other.sourceInfo.adapterId.LowPart == p.sourceInfo.adapterId.LowPart &&
                other.sourceInfo.adapterId.HighPart == p.sourceInfo.adapterId.HighPart)
                throw std::runtime_error("Per-monitor Windows scaling requires extended displays, not cloning.");
        GetScalePacket packet{};
        packet.header = {GetScaleType, sizeof(packet), p.sourceInfo.adapterId, p.sourceInfo.id};
        checkWindows(DisplayConfigGetDeviceInfo(&packet.header), "Read per-monitor Windows scaling");
        return packet;
    }
    throw std::runtime_error("Target monitor is no longer active for Windows scaling.");
}
}
WindowsScaleInfo windowsScale(const std::wstring& key) {
    const auto packet = readScale(key);
    return decodeWindowsScale(packet.minimum, packet.current, packet.maximum);
}
void setWindowsScale(const std::wstring& key, uint32_t percent) {
    // Query after any mode/primary change: both the source ID and recommended/max scale can change.
    const auto current = readScale(key);
    const auto info = decodeWindowsScale(current.minimum, current.current, current.maximum);
    const auto relative = relativeWindowsScale(info, percent); // Reject, never clamp to a different percentage.
    if (info.current == percent) return;
    SetScalePacket packet{};
    packet.header = {SetScaleType, sizeof(packet), current.header.adapterId, current.header.id};
    packet.relative = relative;
    checkWindows(DisplayConfigSetDeviceInfo(&packet.header), "Apply per-monitor Windows scaling");
}
}
