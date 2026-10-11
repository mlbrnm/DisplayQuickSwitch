// Exercise production DPI packet/source routing without calling real CCD APIs or the registry.
#include "windows_scaling.h"
#include "windows_scaling_test_api.h"
#include <iostream>
#include <functional>
#include <cstring>

namespace fakeDpi {
int failures = 0, writes = 0, relative = 0;
bool custom = false, cloned = false, unavailable = false;
bool bufferRace = false;
UINT32 sourceId = 17;
uint32_t writtenSource = 0;
LONG registryStatus = ERROR_SUCCESS, setStatus = ERROR_SUCCESS;
struct GetPacket { DISPLAYCONFIG_DEVICE_INFO_HEADER header; int32_t min, current, max; };
struct SetPacket { DISPLAYCONFIG_DEVICE_INFO_HEADER header; int32_t relative; };
void expect(bool value, const char* label) { if (!value) { std::cerr << "FAIL: " << label << '\n'; ++failures; } }
void rejects(const std::function<void()>& fn, const char* label) { try { fn(); expect(false, label); } catch (const std::exception&) {} }
void reset() {
    writes = relative = 0; custom = cloned = unavailable = bufferRace = false;
    sourceId = 17; writtenSource = 0; registryStatus = setStatus = ERROR_SUCCESS;
}
}
namespace qs::testDpi {
LONG bufferSizes(UINT32 flags, UINT32* paths, UINT32* modes) {
    fakeDpi::expect(flags == QDC_ONLY_ACTIVE_PATHS, "only active paths queried");
    *paths = 2; *modes = 0; return ERROR_SUCCESS;
}
LONG query(UINT32, UINT32* count, DISPLAYCONFIG_PATH_INFO* paths, UINT32*, DISPLAYCONFIG_MODE_INFO*, DISPLAYCONFIG_TOPOLOGY_ID*) {
    if (fakeDpi::bufferRace) { fakeDpi::bufferRace = false; return ERROR_INSUFFICIENT_BUFFER; }
    *count = 2;
    for (size_t i = 0; i < 2; ++i) {
        paths[i] = {};
        paths[i].sourceInfo.adapterId = {123, 456};
        paths[i].sourceInfo.id = fakeDpi::sourceId + static_cast<UINT32>(fakeDpi::cloned ? 0 : i);
        paths[i].targetInfo.adapterId = {123, 456};
        paths[i].targetInfo.id = static_cast<UINT32>(i + 91); // Deliberately NOT source IDs.
    }
    return ERROR_SUCCESS;
}
LONG getDevice(DISPLAYCONFIG_DEVICE_INFO_HEADER* header) {
    if (header->type == DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME) {
        auto packet = reinterpret_cast<DISPLAYCONFIG_TARGET_DEVICE_NAME*>(header);
        wcscpy_s(packet->monitorDevicePath, header->id == 91 ? L"LCD" : L"CRT");
        return ERROR_SUCCESS;
    }
    fakeDpi::expect(static_cast<int>(header->type) == -3 && header->size == 32, "get DPI packet type and size");
    fakeDpi::expect(header->adapterId.LowPart == 123 && header->adapterId.HighPart == 456, "correct source adapter LUID");
    fakeDpi::expect(header->id == fakeDpi::sourceId || header->id == fakeDpi::sourceId + 1, "query uses source ID, not target ID");
    if (fakeDpi::unavailable) return ERROR_NOT_SUPPORTED;
    auto packet = reinterpret_cast<fakeDpi::GetPacket*>(header);
    packet->min = -2; packet->current = fakeDpi::relative; packet->max = 5; // Recommended 150%, max 300%.
    return ERROR_SUCCESS;
}
LONG setDevice(DISPLAYCONFIG_DEVICE_INFO_HEADER* header) {
    fakeDpi::expect(static_cast<int>(header->type) == -4 && header->size == 24, "set DPI packet type and size");
    ++fakeDpi::writes; fakeDpi::writtenSource = header->id;
    fakeDpi::relative = reinterpret_cast<fakeDpi::SetPacket*>(header)->relative;
    return fakeDpi::setStatus;
}
LSTATUS registry(HKEY, LPCWSTR, LPCWSTR value, DWORD, LPDWORD, PVOID data, LPDWORD) {
    fakeDpi::expect(wcscmp(value, L"Win8DpiScaling") == 0, "only reads global custom-scaling flag");
    *static_cast<DWORD*>(data) = fakeDpi::custom ? 1 : 0;
    return fakeDpi::registryStatus;
}
}
int main() {
    using namespace qs;
    using namespace fakeDpi;
    try {
        reset(); bufferRace = true;
        const auto info = windowsScale(L"lcd");
        expect(info.current == 150 && info.recommended == 150 && info.maximum == 300, "read standard Windows scale with query retry");
        setWindowsScale(L"lcd", 125);
        expect(writes == 1 && writtenSource == 17 && relative == -1, "set absolute percent using signed relative source offset");
        expect(windowsScale(L"lcd").current == 125, "readback after setting");
        setWindowsScale(L"lcd", 125); expect(writes == 1, "no-op skips a Windows scaling write");
        sourceId = 42; setWindowsScale(L"crt", 200);
        expect(writtenSource == 43 && relative == 2, "source ID freshly resolved after topology change");
        rejects([] { setWindowsScale(L"lcd", 350); }, "scale above maximum rejected without clamping");
        expect(writes == 2, "unsupported percentage does not write");
        rejects([] { setWindowsScale(L"missing", 100); }, "disconnected monitor rejected");
        cloned = true; rejects([] { setWindowsScale(L"crt", 100); }, "cloned source rejected");
        cloned = false; custom = true; rejects([] { setWindowsScale(L"crt", 100); }, "global custom scaling rejected");
        expect(writes == 2, "cloning/custom scaling cause no writes");
        custom = false; unavailable = true; rejects([] { windowsScale(L"crt"); }, "unavailable undocumented API reported");
        unavailable = false; registryStatus = ERROR_ACCESS_DENIED; rejects([] { windowsScale(L"crt"); }, "registry-read failure not treated as no custom scaling");
        registryStatus = ERROR_FILE_NOT_FOUND; expect(windowsScale(L"crt").current == 200, "missing custom-scaling value is allowed");
        registryStatus = ERROR_SUCCESS; setStatus = ERROR_ACCESS_DENIED;
        rejects([] { setWindowsScale(L"crt", 175); }, "Windows setter error propagates for transaction rollback");
    } catch (const std::exception& e) { std::cerr << "Unexpected: " << e.what() << '\n'; ++failures; }
    if (!failures) std::cout << "Windows scaling API tests passed. All CCD/registry APIs are fake; no real settings changed.\n";
    return failures ? 1 : 0;
}
