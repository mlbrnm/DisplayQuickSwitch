#include "backend.h"
#include "windows_scaling.h"
#include <windows.h>
#include <nvapi.h>
#include <NvApiDriverSettings.h>
#include <algorithm>
#include <sstream>
#include <thread>
#include <chrono>

namespace qs {
#ifdef QS_TEST_BACKEND
// Test-only seams; production always uses Windows/NVIDIA enumeration.
std::vector<Monitor> testMonitors();
uint32_t testDisplayId(const Monitor&);
#endif
namespace {
void check(NvAPI_Status status, const char* operation) {
    if (status == NVAPI_OK) return;
    NvAPI_ShortString message{};
    NvAPI_GetErrorMessage(status, message);
    throw std::runtime_error(std::string(operation) + ": " + message + " (" + std::to_string(status) + ")");
}
struct Api {
    Api() { check(NvAPI_Initialize(), "Initialize NVIDIA API"); }
    ~Api() { NvAPI_Unload(); }
};
struct Drs {
    NvDRSSessionHandle session{};
    NvDRSProfileHandle profile{};
    Drs() {
        check(NvAPI_DRS_CreateSession(&session), "Create driver-settings session");
        try {
            check(NvAPI_DRS_LoadSettings(session), "Load driver settings");
            check(NvAPI_DRS_GetBaseProfile(session, &profile), "Get global driver profile");
        } catch (...) { NvAPI_DRS_DestroySession(session); throw; }
    }
    ~Drs() { NvAPI_DRS_DestroySession(session); }
    Drs(const Drs&) = delete;
    bool get(NvU32 id, NVDRS_SETTING& s) {
        s = {}; s.version = NVDRS_SETTING_VER;
        auto r = NvAPI_DRS_GetSetting(session, profile, id, &s);
        if (r == NVAPI_SETTING_NOT_FOUND) return false;
        check(r, "Read global driver setting");
        if (s.settingType != NVDRS_DWORD_TYPE) throw std::runtime_error("Expected a DWORD driver setting.");
        return true;
    }
    void set(NvU32 id, NvU32 value) {
        NVDRS_SETTING s{}; s.version = NVDRS_SETTING_VER;
        s.settingId = id; s.settingType = NVDRS_DWORD_TYPE; s.u32CurrentValue = value;
        check(NvAPI_DRS_SetSetting(session, profile, &s), "Set global driver setting");
    }
    void save() { check(NvAPI_DRS_SaveSettings(session), "Save global driver settings"); }
};
// Own the complete topology. Raw NVAPI pointers are rebuilt whenever the vectors move/copy.
struct Topology {
    std::vector<NV_DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<NV_DISPLAYCONFIG_SOURCE_MODE_INFO> sources;
    std::vector<std::vector<NV_DISPLAYCONFIG_PATH_TARGET_INFO>> targets;
    std::vector<std::vector<NV_DISPLAYCONFIG_PATH_ADVANCED_TARGET_INFO>> details;
    Topology() = default;
    Topology(const Topology& other) : paths(other.paths), sources(other.sources), targets(other.targets), details(other.details) { link(); }
    Topology& operator=(const Topology& other) {
        paths = other.paths; sources = other.sources; targets = other.targets; details = other.details; link(); return *this;
    }
    void link() {
        for (size_t i = 0; i < paths.size(); ++i) {
            paths[i].sourceModeInfo = &sources[i];
            paths[i].targetInfo = targets[i].data();
            for (size_t j = 0; j < targets[i].size(); ++j) targets[i][j].details = &details[i][j];
        }
    }
    static Topology read() {
        Topology t;
        NvU32 count = 0;
        check(NvAPI_DISP_GetDisplayConfig(&count, nullptr), "Count active display paths");
        if (!count || count > 64) throw std::runtime_error("Unexpected number of display paths.");
        t.paths.resize(count);
        for (auto& p : t.paths) p.version = NV_DISPLAYCONFIG_PATH_INFO_VER;
        check(NvAPI_DISP_GetDisplayConfig(&count, t.paths.data()), "Read display path sizes");
        if (count != t.paths.size()) throw std::runtime_error("Display topology changed; try again.");
        t.sources.resize(count); t.targets.resize(count); t.details.resize(count);
        for (size_t i = 0; i < count; ++i) {
            if (t.paths[i].IsNonNVIDIAAdapter)
                throw std::runtime_error("This version requires all active displays to be driven by NVIDIA. Mixed-GPU topology is not supported.");
            if (t.paths[i].targetInfoCount != 1)
                throw std::runtime_error("Use extended displays, not cloned displays or Surround, with this version.");
            t.targets[i].resize(1); t.details[i].resize(1);
            t.details[i][0].version = NV_DISPLAYCONFIG_PATH_ADVANCED_TARGET_INFO_VER;
        }
        t.link();
        check(NvAPI_DISP_GetDisplayConfig(&count, t.paths.data()), "Read full display configuration");
        if (count != t.paths.size()) throw std::runtime_error("Display topology changed; try again.");
        return t;
    }
    size_t find(NvU32 id) const {
        for (size_t i = 0; i < targets.size(); ++i) if (targets[i][0].displayId == id) return i;
        throw std::runtime_error("The target monitor is no longer active.");
    }
    void validateOnly() {
        link();
        check(NvAPI_DISP_SetDisplayConfig(static_cast<NvU32>(paths.size()), paths.data(), NV_DISPLAYCONFIG_VALIDATE_ONLY),
              "Validate display configuration (no changes applied)");
    }
    void write() {
        link();
        // No DRIVER_RELOAD_ALLOWED: never reset the driver to force a preset to work.
        check(NvAPI_DISP_SetDisplayConfig(static_cast<NvU32>(paths.size()), paths.data(), NV_DISPLAYCONFIG_SAVE_TO_PERSISTENCE),
              "Apply display configuration");
    }
};
Topology readSettled() {
    // Finite retry only during a user-requested operation, not a background poll.
    for (int i = 0; ; ++i) {
        try { return Topology::read(); }
        catch (...) { if (i == 9) throw; std::this_thread::sleep_for(std::chrono::milliseconds(200)); }
    }
}
void verifyWindowsScale(const std::wstring& key, uint32_t percent) {
    for (int i = 0; ; ++i) {
        try {
            if (windowsScale(key).current == percent) return;
            throw std::runtime_error("Windows scaling did not pass readback verification.");
        } catch (...) {
            if (i == 9) throw;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}
NvU32 displayId(const Monitor& m) {
#ifdef QS_TEST_BACKEND
    return testDisplayId(m);
#else
    char name[128]{};
    if (!WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, m.gdiName.c_str(), -1, name, sizeof(name), nullptr, nullptr))
        throw std::runtime_error("Invalid Windows display name.");
    NvU32 id = 0;
    check(NvAPI_DISP_GetDisplayIdByDisplayName(name, &id), "Resolve NVIDIA display ID");
    return id;
#endif
}
std::vector<std::pair<NvU32, NvU32>> desiredSettings(const Preset& p) {
    std::vector<std::pair<NvU32, NvU32>> out;
    if (p.gsync != Gsync::unchanged) {
        const auto mode = static_cast<NvU32>(p.gsync);
        out.emplace_back(VRRFEATUREINDICATOR_ID, mode ? VRRFEATUREINDICATOR_ENABLED : VRRFEATUREINDICATOR_DISABLED);
        out.emplace_back(VRRREQUESTSTATE_ID, mode);
        out.emplace_back(VRR_MODE_ID, mode);
        out.emplace_back(VRR_APP_OVERRIDE_ID, mode ? VRR_APP_OVERRIDE_ALLOW : VRR_APP_OVERRIDE_FORCE_OFF);
        out.emplace_back(VRR_APP_OVERRIDE_REQUEST_STATE_ID, mode ? VRR_APP_OVERRIDE_REQUEST_STATE_ALLOW : VRR_APP_OVERRIDE_REQUEST_STATE_FORCE_OFF);
    }
    if (p.vsync != Vsync::unchanged) {
        NvU32 direct = VSYNCMODE_PASSIVE;
        NvU32 opengl = OGL_DEFAULT_SWAP_INTERVAL_VSYNC_ONE; // No force bits: honor application requests.
        if (p.vsync == Vsync::off) { direct = VSYNCMODE_FORCEOFF; opengl = static_cast<NvU32>(OGL_DEFAULT_SWAP_INTERVAL_FORCE_OFF); }
        if (p.vsync == Vsync::on) { direct = VSYNCMODE_FORCEON; opengl = OGL_DEFAULT_SWAP_INTERVAL_FORCE_ON | 1; }
        out.emplace_back(VSYNCMODE_ID, direct);
        out.emplace_back(OGL_DEFAULT_SWAP_INTERVAL_ID, opengl);
        out.emplace_back(OGL_DEFAULT_SWAP_INTERVAL_SIGN_ID, OGL_DEFAULT_SWAP_INTERVAL_SIGN_POSITIVE);
    }
    if (p.maxFps >= 0) out.emplace_back(FRL_FPS_ID, static_cast<NvU32>(p.maxFps));
    return out;
}
bool sameKey(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }
Monitor resolve(const std::wstring& key) {
    for (const auto& m : monitors()) if (sameKey(m.key, key)) return m;
    throw std::runtime_error("Preset monitor is disconnected or disabled. Enable it in Windows first, then try again.");
}
}

std::vector<Monitor> monitors() {
#ifdef QS_TEST_BACKEND
    return testMonitors();
#else
    std::vector<Monitor> out;
    UINT32 pathCount = 0, modeCount = 0;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    LONG result = ERROR_INSUFFICIENT_BUFFER;
    for (int attempt = 0; attempt < 5 && result == ERROR_INSUFFICIENT_BUFFER; ++attempt) {
        result = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
        if (result != ERROR_SUCCESS) break;
        paths.resize(pathCount); modes.resize(modeCount);
        result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
    }
    if (result != ERROR_SUCCESS) throw std::runtime_error("Cannot enumerate active Windows displays (" + std::to_string(result) + ").");
    paths.resize(pathCount);
    for (const auto& p : paths) {
        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof(target), p.targetInfo.adapterId, p.targetInfo.id};
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source), p.sourceInfo.adapterId, p.sourceInfo.id};
        if (DisplayConfigGetDeviceInfo(&target.header) || DisplayConfigGetDeviceInfo(&source.header))
            throw std::runtime_error("Cannot identify an active monitor.");
        if (!target.monitorDevicePath[0]) throw std::runtime_error("An active monitor has no stable device identity.");
        Monitor m{target.monitorDevicePath, target.monitorFriendlyDeviceName, source.viewGdiDeviceName, false};
        if (m.name.empty()) m.name = L"Monitor";
        DISPLAY_DEVICEW device{}; device.cb = sizeof(device);
        for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &device, 0); ++i) {
            if (m.gdiName == device.DeviceName) m.primary = (device.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
            device = {}; device.cb = sizeof(device);
        }
        out.push_back(std::move(m));
    }
    return out;
#endif
}
Preset capture(const Monitor& m) {
    Api api;
    auto t = Topology::read();
    const auto i = t.find(displayId(m));
    Preset p;
    p.name = m.name; p.monitor = m.key; p.primary = true;
    p.width = t.sources[i].resolution.width; p.height = t.sources[i].resolution.height;
    p.refreshMilliHz = t.details[i][0].refreshRate1K;
    p.scaling = static_cast<Scaling>(t.details[i][0].scaling);
    if (p.scaling != Scaling::none && p.scaling != Scaling::stretch && p.scaling != Scaling::aspect && p.scaling != Scaling::integer)
        p.scaling = Scaling::unchanged;
    Drs drs;
    NVDRS_SETTING s{};
    if (drs.get(VRRFEATUREINDICATOR_ID, s) && s.u32CurrentValue == 0) p.gsync = Gsync::off;
    else if (drs.get(VRRREQUESTSTATE_ID, s) && s.u32CurrentValue <= 2) p.gsync = static_cast<Gsync>(s.u32CurrentValue);
    if (drs.get(VSYNCMODE_ID, s)) {
        if (s.u32CurrentValue == VSYNCMODE_PASSIVE) p.vsync = Vsync::application;
        if (s.u32CurrentValue == VSYNCMODE_FORCEOFF) p.vsync = Vsync::off;
        if (s.u32CurrentValue == VSYNCMODE_FORCEON) p.vsync = Vsync::on;
    }
    if (drs.get(FRL_FPS_ID, s) && s.u32CurrentValue <= 1023) p.maxFps = static_cast<int>(s.u32CurrentValue);
    // DPI support is optional; inability to capture it must not break existing presets.
    try { p.windowsScalePercent = windowsScale(m.key).current; } catch (const std::exception&) {}
    return p;
}
struct Transaction::Impl {
    struct Setting { NvU32 id, value; bool userOverride, existed; };
    Topology before;
    std::vector<Setting> settings;
    bool displayDirty = false, driverDirty = false;
    bool windowsScaleDirty = false;
    std::wstring windowsScaleKey;
    uint32_t previousWindowsScale = 0;
};
Transaction::Transaction() : impl(std::make_unique<Impl>()) {}
Transaction::~Transaction() = default;
bool Transaction::needsRestore() const { return impl->displayDirty || impl->driverDirty || impl->windowsScaleDirty; }
void Transaction::apply(const Preset& p) {
    validate(p);
    if (needsRestore()) throw std::runtime_error("Transaction already contains applied settings.");
    Api api;
    auto monitor = resolve(p.monitor);
    const auto id = displayId(monitor);
    impl->before = Topology::read();
    auto target = impl->before;
    const auto selected = target.find(id);
    if (p.windowsScalePercent) {
        impl->windowsScaleKey = monitor.key;
        const auto info = windowsScale(monitor.key); // Snapshot/fail before making ANY changes.
        impl->previousWindowsScale = info.current;
        // A resolution change can expand the supported range; check that range again after modeset.
        if (!p.width) relativeWindowsScale(info, p.windowsScalePercent);
    }
    if (p.primary) {
        std::vector<Position> positions;
        for (auto& s : target.sources) positions.push_back({s.position.x, s.position.y});
        auto rebased = rebasePositions(positions, selected);
        for (size_t i = 0; i < target.sources.size(); ++i) {
            target.sources[i].position = {rebased[i].x, rebased[i].y};
            target.sources[i].bGDIPrimary = i == selected;
        }
    }
    if (p.width) { target.sources[selected].resolution.width = p.width; target.sources[selected].resolution.height = p.height; }
    auto& detail = target.details[selected][0];
    if (p.refreshMilliHz) detail.refreshRate1K = p.refreshMilliHz;
    if (p.width || p.refreshMilliHz) detail.timingOverride = NV_TIMING_OVERRIDE_AUTO;
    if (p.scaling != Scaling::unchanged) detail.scaling = static_cast<NV_SCALING>(p.scaling);
    for (auto& s : target.sources) s.colorFormat = NV_FORMAT_UNKNOWN;
    target.validateOnly();
    const auto desired = desiredSettings(p);
    impl->settings.clear();
    // Stage all DRS edits in memory before any persistent write or display change.
    Drs drs;
    for (auto [key, value] : desired) {
        NVDRS_SETTING old{};
        bool exists = drs.get(key, old);
        impl->settings.push_back({key, exists ? old.u32CurrentValue : 0,
            exists && old.isCurrentPredefined == 0 && old.settingLocation == NVDRS_CURRENT_PROFILE_LOCATION, exists});
        drs.set(key, value);
    }
    try {
        // The order is intentional: establish the new primary/display mode, then global policy.
        impl->displayDirty = true; // A failing driver call can partially apply.
        impl->windowsScaleDirty = p.windowsScalePercent != 0; // Modeset can itself alter DPI.
        target.write();
        if (!desired.empty()) { impl->driverDirty = true; drs.save(); }
        const auto actual = readSettled();
        const auto idx = actual.find(id);
        if (actual.paths.size() != target.paths.size()) throw std::runtime_error("Driver changed the number of active displays.");
        for (size_t i = 0; i < target.paths.size(); ++i) {
            auto j = actual.find(target.targets[i][0].displayId);
            auto& a = actual.sources[j]; auto& expected = target.sources[i];
            if (a.resolution.width != expected.resolution.width || a.resolution.height != expected.resolution.height ||
                a.position.x != expected.position.x || a.position.y != expected.position.y || a.bGDIPrimary != expected.bGDIPrimary)
                throw std::runtime_error("Driver changed the requested desktop layout or another monitor's resolution.");
            if (i != selected && (actual.details[j][0].scaling != target.details[i][0].scaling ||
                std::abs(static_cast<int64_t>(actual.details[j][0].refreshRate1K) - target.details[i][0].refreshRate1K) > 500))
                throw std::runtime_error("Driver changed another monitor's scaling or refresh rate.");
        }
        if (p.primary && !actual.sources[idx].bGDIPrimary) throw std::runtime_error("Driver did not switch the primary display.");
        if (p.width && (actual.sources[idx].resolution.width != p.width || actual.sources[idx].resolution.height != p.height))
            throw std::runtime_error("Driver did not accept the requested resolution.");
        if (p.refreshMilliHz && std::abs(static_cast<int64_t>(actual.details[idx][0].refreshRate1K) - p.refreshMilliHz) > 500)
            throw std::runtime_error("Driver did not accept the requested refresh rate (tolerance 0.5 Hz).");
        if (p.scaling != Scaling::unchanged && actual.details[idx][0].scaling != static_cast<NV_SCALING>(p.scaling))
            throw std::runtime_error("Driver did not accept the requested scaling mode.");
        Drs readback;
        for (auto [key, value] : desired) {
            NVDRS_SETTING current{};
            if (!readback.get(key, current) || current.u32CurrentValue != value)
                throw std::runtime_error("A global driver setting did not pass readback verification.");
        }
        if (p.windowsScalePercent) {
            setWindowsScale(impl->windowsScaleKey, p.windowsScalePercent);
            verifyWindowsScale(impl->windowsScaleKey, p.windowsScalePercent);
        }
    } catch (const std::exception& e) {
        auto message = std::string(e.what());
        try { restore(); message += "\nPrevious settings restored."; }
        catch (const std::exception& r) { message += "\nRESTORATION FAILED: "; message += r.what(); }
        throw std::runtime_error(message);
    }
}
void Transaction::restore() {
    std::string errors;
    // Try all three even when one fails. DPI restoration must not depend on NVAPI initialization.
    if (impl->displayDirty) {
        try {
            Api api;
            impl->before.write();
            auto actual = readSettled();
            for (size_t i = 0; i < impl->before.paths.size(); ++i) {
                auto idx = actual.find(impl->before.targets[i][0].displayId);
                auto& a = actual.sources[idx]; auto& b = impl->before.sources[i];
                if (a.resolution.width != b.resolution.width || a.resolution.height != b.resolution.height ||
                    a.position.x != b.position.x || a.position.y != b.position.y || a.bGDIPrimary != b.bGDIPrimary ||
                    actual.details[idx][0].scaling != impl->before.details[i][0].scaling ||
                    std::abs(static_cast<int64_t>(actual.details[idx][0].refreshRate1K) - impl->before.details[i][0].refreshRate1K) > 500)
                    throw std::runtime_error("Previous display state did not pass readback.");
            }
            impl->displayDirty = false;
        } catch (const std::exception& e) { errors += e.what(); }
    }
    if (impl->driverDirty) {
        try {
            Api api;
            Drs drs;
            for (auto& s : impl->settings) {
                if (s.userOverride) drs.set(s.id, s.value);
                else {
                    auto r = NvAPI_DRS_DeleteProfileSetting(drs.session, drs.profile, s.id);
                    if (r != NVAPI_SETTING_NOT_FOUND) check(r, "Remove temporary global override");
                }
            }
            drs.save();
            Drs readback;
            for (auto& s : impl->settings) {
                NVDRS_SETTING actual{};
                bool exists = readback.get(s.id, actual);
                if ((s.existed && (!exists || actual.u32CurrentValue != s.value)) ||
                    (s.userOverride && actual.isCurrentPredefined != 0) ||
                    (!s.userOverride && exists && actual.isCurrentPredefined == 0 && actual.settingLocation == NVDRS_CURRENT_PROFILE_LOCATION))
                    throw std::runtime_error("Previous global driver state did not pass readback.");
            }
            impl->driverDirty = false;
        } catch (const std::exception& e) { if (!errors.empty()) errors += "\n"; errors += e.what(); }
    }
    // Restore topology FIRST so the old percentage's range/recommended scale is valid again.
    if (impl->windowsScaleDirty) {
        try {
            setWindowsScale(impl->windowsScaleKey, impl->previousWindowsScale);
            verifyWindowsScale(impl->windowsScaleKey, impl->previousWindowsScale);
            impl->windowsScaleDirty = false;
        } catch (const std::exception& e) { if (!errors.empty()) errors += "\n"; errors += e.what(); }
    }
    if (!errors.empty()) throw std::runtime_error(errors);
}
std::wstring diagnostics() {
    std::wostringstream out;
    out << L"Nvidia Quick Switch 0.2.0 — read-only diagnostics\r\n\r\n";
    Api api;
    NvU32 version = 0; NvAPI_ShortString branch{};
    check(NvAPI_SYS_GetDriverAndBranchVersion(&version, branch), "Read driver version");
    out << L"Driver: " << version / 100 << L"." << version % 100 << L"\r\n";
    NvPhysicalGpuHandle handles[NVAPI_MAX_PHYSICAL_GPUS]{}; NvU32 count = 0;
    check(NvAPI_EnumPhysicalGPUs(handles, &count), "Enumerate NVIDIA GPUs");
    for (NvU32 i = 0; i < count; ++i) {
        NvAPI_ShortString name{}; check(NvAPI_GPU_GetFullName(handles[i], name), "Read GPU name");
        out << L"GPU: " << std::wstring(name, name + strlen(name)) << L"\r\n";
    }
    for (const auto& m : monitors()) {
        auto p = capture(m);
        out << L"\r\n" << m.name << (m.primary ? L" [primary]" : L"") << L" (" << m.gdiName << L")\r\n"
            << m.key << L"\r\n" << p.width << L" x " << p.height << L" @ " << formatRefresh(p.refreshMilliHz)
            << L" Hz; scaling=" << static_cast<int>(p.scaling) << L"\r\n"
            << L"Windows scaling: " << (p.windowsScalePercent ? std::to_wstring(p.windowsScalePercent) + L"%" : L"unavailable (custom scaling or unsupported API)") << L"\r\n"
            << L"Global G-SYNC=" << static_cast<int>(p.gsync) << L"; V-SYNC=" << static_cast<int>(p.vsync)
            << L"; FPS=" << p.maxFps << L"\r\n";
    }
    out << L"\r\nG-SYNC readback is driver policy, not proof of active VRR.\r\nDSR/DLDSR factor switching is not implemented.\r\n";
    return out.str();
}
}
