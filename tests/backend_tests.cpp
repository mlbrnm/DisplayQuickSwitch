// Fake NVAPI implementation: this executable does NOT link the NVIDIA library.
// Tests cannot touch a real display or driver profile.
#include "backend.h"
#include "windows_scaling.h"
#include <windows.h>
#include <nvapi.h>
#include <NvApiDriverSettings.h>
#include <map>
#include <array>
#include <iostream>
#include <functional>
#include <cstring>

namespace fake {
using Settings = std::map<NvU32, NvU32>;
struct Session { Settings settings; };
struct Display { NV_DISPLAYCONFIG_SOURCE_MODE_INFO source{}; NV_DISPLAYCONFIG_PATH_ADVANCED_TARGET_INFO detail{}; };
std::array<Display, 2> displays;
Settings overrides;
Settings defaults;
int displayWrites = 0, profileWrites = 0, calls = 0;
bool failValidation = false, failDisplayWrite = false, failSave = false, unsupportedSetting = false;
bool wrongOtherMonitor = false, failRestoreDisplay = false;
std::map<std::wstring, uint32_t> scales;
int scaleReads = 0, scaleWrites = 0;
bool scalingUnavailable = false, failScalingWrite = false, failScalingRestore = false, ignoreScalingWrite = false;
void reset() {
    displays = {};
    displays[0].source.resolution = {3840, 2160, 32}; displays[0].source.bGDIPrimary = 1;
    displays[1].source.resolution = {1600, 1200, 32}; displays[1].source.position = {3840, 0};
    for (auto& d : displays) { d.detail.version = NV_DISPLAYCONFIG_PATH_ADVANCED_TARGET_INFO_VER; d.detail.scaling = NV_SCALING_GPU_SCANOUT_TO_NATIVE; }
    displays[0].detail.refreshRate1K = 120000; displays[1].detail.refreshRate1K = 85000;
    overrides.clear(); defaults.clear();
    defaults[VRRFEATUREINDICATOR_ID] = 1; defaults[VRRREQUESTSTATE_ID] = 1; defaults[VRR_MODE_ID] = 1;
    defaults[VRR_APP_OVERRIDE_ID] = 0; defaults[VRR_APP_OVERRIDE_REQUEST_STATE_ID] = 0;
    defaults[VSYNCMODE_ID] = VSYNCMODE_PASSIVE; defaults[OGL_DEFAULT_SWAP_INTERVAL_ID] = 1;
    defaults[OGL_DEFAULT_SWAP_INTERVAL_SIGN_ID] = 0; defaults[FRL_FPS_ID] = 0;
    displayWrites = profileWrites = calls = 0;
    failValidation = failDisplayWrite = failSave = unsupportedSetting = wrongOtherMonitor = failRestoreDisplay = false;
    scales = {{L"lcd", 150}, {L"crt", 100}};
    scaleReads = scaleWrites = 0;
    scalingUnavailable = failScalingWrite = failScalingRestore = ignoreScalingWrite = false;
}
Session* session(NvDRSSessionHandle h) { return reinterpret_cast<Session*>(h); }
}
namespace qs {
std::vector<Monitor> testMonitors() { return {{L"lcd", L"LCD", L"DISPLAY1", true}, {L"crt", L"CRT", L"DISPLAY2", false}}; }
uint32_t testDisplayId(const Monitor& m) { return m.key == L"lcd" ? 1 : 2; }
WindowsScaleInfo windowsScale(const std::wstring& key) {
    ++fake::scaleReads;
    if (fake::scalingUnavailable) throw std::runtime_error("Simulated unsupported DPI API / custom scaling.");
    return {fake::scales.at(key), 150, 500};
}
void setWindowsScale(const std::wstring& key, uint32_t percent) {
    const auto info = windowsScale(key); relativeWindowsScale(info, percent);
    if (info.current == percent) return;
    ++fake::scaleWrites;
    if (fake::failScalingRestore && fake::scaleWrites > 1) throw std::runtime_error("Simulated DPI restore failure.");
    if (!fake::ignoreScalingWrite) fake::scales[key] = percent;
    if (fake::failScalingWrite) { fake::failScalingWrite = false; throw std::runtime_error("Simulated partial DPI write failure."); }
}
}
extern "C" {
NvAPI_Status __cdecl NvAPI_Initialize() { ++fake::calls; return NVAPI_OK; }
NvAPI_Status __cdecl NvAPI_Unload() { return NVAPI_OK; }
NvAPI_Status __cdecl NvAPI_GetErrorMessage(NvAPI_Status, NvAPI_ShortString s) { strcpy_s(s, 64, "simulated error"); return NVAPI_OK; }
NvAPI_Status __cdecl NvAPI_DISP_GetDisplayConfig(NvU32* count, NV_DISPLAYCONFIG_PATH_INFO* paths) {
    ++fake::calls;
    *count = 2;
    if (!paths) return NVAPI_OK;
    for (size_t i = 0; i < 2; ++i) {
        paths[i].targetInfoCount = 1; paths[i].sourceId = static_cast<NvU32>(i);
        if (paths[i].sourceModeInfo) *paths[i].sourceModeInfo = fake::displays[i].source;
        if (paths[i].targetInfo) {
            paths[i].targetInfo[0].displayId = static_cast<NvU32>(i + 1);
            *paths[i].targetInfo[0].details = fake::displays[i].detail;
        }
    }
    return NVAPI_OK;
}
NvAPI_Status __cdecl NvAPI_DISP_SetDisplayConfig(NvU32 count, NV_DISPLAYCONFIG_PATH_INFO* paths, NvU32 flags) {
    ++fake::calls;
    if (flags & NV_DISPLAYCONFIG_VALIDATE_ONLY) return fake::failValidation ? NVAPI_ERROR : NVAPI_OK;
    ++fake::displayWrites;
    if (fake::failRestoreDisplay && fake::displayWrites > 1) return NVAPI_ERROR;
    for (NvU32 i = 0; i < count; ++i) {
        auto index = paths[i].targetInfo[0].displayId - 1;
        fake::displays[index].source = *paths[i].sourceModeInfo;
        fake::displays[index].detail = *paths[i].targetInfo[0].details;
    }
    if (fake::wrongOtherMonitor) { fake::wrongOtherMonitor = false; fake::displays[0].source.resolution.width = 640; }
    if (fake::failDisplayWrite) { fake::failDisplayWrite = false; return NVAPI_ERROR; }
    return NVAPI_OK;
}
NvAPI_Status __cdecl NvAPI_DRS_CreateSession(NvDRSSessionHandle* h) { *h = reinterpret_cast<NvDRSSessionHandle>(new fake::Session); return NVAPI_OK; }
NvAPI_Status __cdecl NvAPI_DRS_DestroySession(NvDRSSessionHandle h) { delete fake::session(h); return NVAPI_OK; }
NvAPI_Status __cdecl NvAPI_DRS_LoadSettings(NvDRSSessionHandle h) { fake::session(h)->settings = fake::overrides; return NVAPI_OK; }
NvAPI_Status __cdecl NvAPI_DRS_GetBaseProfile(NvDRSSessionHandle, NvDRSProfileHandle* p) { *p = reinterpret_cast<NvDRSProfileHandle>(1); return NVAPI_OK; }
NvAPI_Status __cdecl NvAPI_DRS_GetSetting(NvDRSSessionHandle h, NvDRSProfileHandle, NvU32 id, NVDRS_SETTING* s) {
    auto& values = fake::session(h)->settings;
    s->settingId = id; s->settingType = NVDRS_DWORD_TYPE;
    s->settingLocation = NVDRS_CURRENT_PROFILE_LOCATION;
    if (values.contains(id)) { s->u32CurrentValue = values.at(id); s->isCurrentPredefined = 0; }
    else if (fake::defaults.contains(id)) { s->u32CurrentValue = fake::defaults.at(id); s->isCurrentPredefined = 1; }
    else return NVAPI_SETTING_NOT_FOUND;
    return NVAPI_OK;
}
NvAPI_Status __cdecl NvAPI_DRS_SetSetting(NvDRSSessionHandle h, NvDRSProfileHandle, NVDRS_SETTING* s) {
    if (fake::unsupportedSetting) return NVAPI_NOT_SUPPORTED;
    fake::session(h)->settings[s->settingId] = s->u32CurrentValue; return NVAPI_OK;
}
NvAPI_Status __cdecl NvAPI_DRS_DeleteProfileSetting(NvDRSSessionHandle h, NvDRSProfileHandle, NvU32 id) {
    fake::session(h)->settings.erase(id); return NVAPI_OK;
}
NvAPI_Status __cdecl NvAPI_DRS_SaveSettings(NvDRSSessionHandle h) {
    ++fake::profileWrites; fake::overrides = fake::session(h)->settings;
    if (fake::failSave) { fake::failSave = false; return NVAPI_ERROR; }
    return NVAPI_OK;
}
NvAPI_Status __cdecl NvAPI_SYS_GetDriverAndBranchVersion(NvU32* v, NvAPI_ShortString s) { *v = 61742; strcpy_s(s, 64, "test"); return NVAPI_OK; }
NvAPI_Status __cdecl NvAPI_EnumPhysicalGPUs(NvPhysicalGpuHandle*, NvU32* count) { *count = 0; return NVAPI_OK; }
NvAPI_Status __cdecl NvAPI_GPU_GetFullName(NvPhysicalGpuHandle, NvAPI_ShortString s) { strcpy_s(s, 64, "test"); return NVAPI_OK; }
}
namespace {
int failures = 0;
void expect(bool condition, const char* label) { if (!condition) { std::cerr << "FAIL: " << label << '\n'; ++failures; } }
void rejects(const std::function<void()>& fn, const char* label) {
    try { fn(); expect(false, label); } catch (const std::exception&) {}
}
qs::Preset crtPreset() {
    qs::Preset p; p.name = L"CRT"; p.monitor = L"crt"; p.width = 1600; p.height = 1200; p.refreshMilliHz = 90000;
    p.gsync = qs::Gsync::off; p.vsync = qs::Vsync::on; p.scaling = qs::Scaling::aspect; p.maxFps = 87; return p;
}
void originalDisplay(const char* label) {
    expect(fake::displays[0].source.bGDIPrimary && !fake::displays[1].source.bGDIPrimary &&
           fake::displays[0].source.position.x == 0 && fake::displays[1].source.position.x == 3840 &&
           fake::displays[0].source.resolution.width == 3840 && fake::displays[1].detail.refreshRate1K == 85000, label);
}
}
int main() {
    try {
        fake::reset();
        { qs::Transaction tx; expect(fake::calls == 0, "creating a transaction does not initialize the driver"); }
        fake::overrides[FRL_FPS_ID] = 116;
        fake::overrides[0x12345678] = 55; // Unrelated user setting must survive.
        const auto originalSettings = fake::overrides;
        qs::Transaction tx; tx.apply(crtPreset());
        expect(fake::displays[1].source.bGDIPrimary && fake::displays[1].source.position.x == 0 &&
               fake::displays[0].source.position.x == -3840, "switch primary and rebase all displays");
        expect(fake::overrides[VRRFEATUREINDICATOR_ID] == 0 && fake::overrides[VRRREQUESTSTATE_ID] == 0 &&
               fake::overrides[VRR_MODE_ID] == 0 && fake::overrides[VRR_APP_OVERRIDE_ID] == 1 &&
               fake::overrides[VRR_APP_OVERRIDE_REQUEST_STATE_ID] == 1, "global G-SYNC off policy");
        expect(fake::overrides[VSYNCMODE_ID] == VSYNCMODE_FORCEON && fake::overrides[OGL_DEFAULT_SWAP_INTERVAL_ID] == 0x10000001 &&
               fake::overrides[FRL_FPS_ID] == 87, "V-SYNC and max FPS");
        tx.restore(); originalDisplay("restore full display topology");
        expect(fake::overrides == originalSettings && !tx.needsRestore(), "restore user/default provenance without touching unrelated settings");
        fake::reset(); fake::failValidation = true;
        qs::Transaction invalid; rejects([&] { invalid.apply(crtPreset()); }, "reject unsupported mode");
        expect(!fake::displayWrites && !fake::profileWrites, "validation failure causes no writes");
        fake::reset(); fake::unsupportedSetting = true;
        qs::Transaction unsupported; rejects([&] { unsupported.apply(crtPreset()); }, "unsupported setting");
        expect(!fake::displayWrites && !fake::profileWrites, "DRS staging failure causes no writes");
        fake::reset(); fake::failDisplayWrite = true;
        qs::Transaction partial; rejects([&] { partial.apply(crtPreset()); }, "partial display failure");
        originalDisplay("partial display write rolls back");
        expect(fake::overrides.empty() && !partial.needsRestore(), "DRS was not saved after failed display apply");
        fake::reset(); fake::failSave = true;
        qs::Transaction saveFailure; rejects([&] { saveFailure.apply(crtPreset()); }, "partial DRS save failure");
        originalDisplay("DRS save failure restores displays");
        expect(fake::overrides.empty() && !saveFailure.needsRestore(), "DRS save failure restores profile");
        fake::reset(); fake::wrongOtherMonitor = true;
        qs::Transaction changedOther; rejects([&] { changedOther.apply(crtPreset()); }, "unexpected other monitor change");
        originalDisplay("readback mismatch restores displays");
        expect(fake::overrides.empty(), "readback mismatch restores global settings");
        fake::reset(); fake::wrongOtherMonitor = true; fake::failRestoreDisplay = true;
        qs::Transaction failedRestore; rejects([&] { failedRestore.apply(crtPreset()); }, "report failed rollback");
        expect(fake::overrides.empty() && failedRestore.needsRestore(), "restore driver settings even if display rollback fails");
        fake::failRestoreDisplay = false; failedRestore.restore();
        originalDisplay("failed restore can be retried");
        fake::reset();
        auto untouched = crtPreset(); untouched.gsync = qs::Gsync::unchanged; untouched.vsync = qs::Vsync::unchanged; untouched.maxFps = -1;
        qs::Transaction displayOnly; displayOnly.apply(untouched);
        expect(fake::profileWrites == 0 && fake::overrides.empty(), "unchanged driver settings cause no profile writes");
        displayOnly.restore();
        expect(fake::scaleReads == 0 && fake::scaleWrites == 0, "leave Windows scaling unchanged performs no DPI API calls");
        fake::reset();
        auto scaledPreset = crtPreset(); scaledPreset.windowsScalePercent = 125;
        qs::Transaction scaled; scaled.apply(scaledPreset);
        expect(fake::scales.at(L"crt") == 125 && fake::scales.at(L"lcd") == 150, "Windows scaling affects target monitor only");
        scaled.restore();
        expect(fake::scales.at(L"crt") == 100 && fake::scales.at(L"lcd") == 150 && !scaled.needsRestore(), "Windows scaling is restored with the preset");
        fake::reset(); fake::scalingUnavailable = true;
        qs::Transaction unavailable; rejects([&] { unavailable.apply(scaledPreset); }, "unsupported DPI API aborts");
        expect(!fake::displayWrites && !fake::profileWrites && !fake::scaleWrites, "unsupported DPI aborts before any changes");
        // Existing presets continue to work when Windows scaling cannot be queried.
        qs::Transaction unchangedDpi; unchangedDpi.apply(crtPreset()); unchangedDpi.restore();
        expect(fake::scaleReads == 1, "unchanged DPI does not query unsupported API");
        fake::reset(); fake::failScalingWrite = true;
        qs::Transaction partialScale; rejects([&] { partialScale.apply(scaledPreset); }, "partial DPI write fails");
        originalDisplay("partial DPI failure restores topology");
        expect(fake::scales.at(L"crt") == 100 && fake::overrides.empty() && !partialScale.needsRestore(), "partial DPI failure restores all three settings groups");
        fake::reset(); fake::ignoreScalingWrite = true;
        qs::Transaction ignoredScale; rejects([&] { ignoredScale.apply(scaledPreset); }, "DPI readback mismatch fails");
        originalDisplay("DPI readback failure restores topology");
        expect(fake::overrides.empty() && !ignoredScale.needsRestore(), "ignored DPI write does not claim success");
        fake::reset(); fake::failScalingWrite = true; fake::failScalingRestore = true;
        qs::Transaction retryScale; rejects([&] { retryScale.apply(scaledPreset); }, "DPI restore failure reported");
        originalDisplay("DPI restore failure still restores displays");
        expect(fake::overrides.empty() && retryScale.needsRestore(), "DPI-only failed rollback retains retry state");
        fake::failScalingRestore = false; retryScale.restore();
        expect(fake::scales.at(L"crt") == 100 && !retryScale.needsRestore(), "DPI-only restore can be retried");
        fake::reset(); fake::failSave = true; fake::failRestoreDisplay = true;
        qs::Transaction failedModeRestore; rejects([&] { failedModeRestore.apply(scaledPreset); }, "failed display rollback with DPI enabled");
        expect(fake::scales.at(L"crt") == 100 && fake::overrides.empty(), "DPI restoration is attempted even when display restore fails");
        fake::failRestoreDisplay = false; failedModeRestore.restore();
        fake::reset();
        expect(qs::capture(qs::testMonitors()[1]).windowsScalePercent == 100, "capture includes Windows scaling");
        fake::scalingUnavailable = true;
        expect(qs::capture(qs::testMonitors()[1]).windowsScalePercent == 0, "capture gracefully leaves unsupported Windows scaling unchanged");
    } catch (const std::exception& e) { std::cerr << "Unexpected: " << e.what() << '\n'; ++failures; }
    if (!failures) std::cout << "All backend transaction tests passed using fake NVAPI. No real display or driver writes.\n";
    return failures ? 1 : 0;
}
