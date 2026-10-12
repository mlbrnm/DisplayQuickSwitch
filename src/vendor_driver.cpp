#include "vendor_driver.h"
#include <windows.h>
#ifdef QS_ENABLE_AMD_ADLX
#include <ADLX.h>
#include <I3DSettings.h>
#include <IDisplays.h>
#include <IDisplaySettings.h>
#endif
#include <igcl_api.h>
#include <algorithm>
#include <cstring>
#include <cmath>

namespace qs {
#ifdef QS_TEST_VENDOR
FARPROC testVendorProc(const char* name); // Fake SDK functions only; no driver DLL is loaded.
#endif
namespace {
struct Module {
    HMODULE handle{};
    explicit Module(const wchar_t* name) {
#ifdef QS_TEST_VENDOR
        (void)name;
#else
        handle = LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!handle) throw std::runtime_error("The installed graphics driver does not provide the required vendor API DLL.");
#endif
    }
    ~Module() { if (handle) FreeLibrary(handle); }
    template<class T> T get(const char* name) const {
#ifdef QS_TEST_VENDOR
        auto fn = testVendorProc(name);
#else
        auto fn = GetProcAddress(handle, name);
#endif
        if (!fn) throw std::runtime_error(std::string("Driver API entry point unavailable: ") + name);
        return reinterpret_cast<T>(fn);
    }
};
#ifdef QS_ENABLE_AMD_ADLX
void amdCheck(ADLX_RESULT r) {
    if (!ADLX_SUCCEEDED(r)) throw std::runtime_error("AMD ADLX error " + std::to_string(r) + ".");
}
#endif
void intelCheck(ctl_result_t r) {
    if (r != CTL_RESULT_SUCCESS) throw std::runtime_error("Intel IGCL error " + std::to_string(static_cast<uint32_t>(r)) + ".");
}
void require(bool supported, const char* feature) {
    if (!supported) throw std::runtime_error(std::string(feature) + " is unavailable through this GPU/driver API. Edit the preset and leave it unchanged.");
}
void checkRequest(const Preset& p, const DriverCapabilities& c) {
    if (p.scaling != Scaling::unchanged) require(p.scaling == Scaling::integer ? c.integerScaling : c.scaling, "GPU scaling");
    if (p.gsync != Gsync::unchanged) {
        require(c.vrr, "Variable refresh rate");
        if (p.gsync == Gsync::windowed) throw std::runtime_error("This vendor exposes per-display VRR on/off, not NVIDIA's full-screen + windowed policy. Edit the preset first.");
    }
    if (p.vsync != Vsync::unchanged) require(c.vsync, "V-SYNC");
    if (p.maxFps >= 0) {
        require(c.fps, "Frame limiter");
        if (p.maxFps && (p.maxFps < c.minFps || p.maxFps > c.maxFps || (p.maxFps - c.minFps) % c.fpsStep))
            throw std::runtime_error("FPS cap is outside the driver-supported range/step.");
    }
}
class Unavailable final : public VendorDriver {
    std::wstring message;
public:
    explicit Unavailable(std::wstring text) : message(std::move(text)) {}
    DriverCapabilities capabilities() override { DriverCapabilities c; c.notice = message; return c; }
    void capture(Preset&) override {}
    void stage(const Preset& p) override { checkRequest(p, capabilities()); }
};
#ifdef QS_ENABLE_AMD_ADLX
std::string windowsEdid(const std::wstring& key) {
    // Windows monitor interface: \\?\DISPLAY#model#instance#{guid}.
    auto a = key.find(L'#'), b = key.find(L'#', a == std::wstring::npos ? 0 : a + 1);
    auto c = key.find(L'#', b == std::wstring::npos ? 0 : b + 1);
    if (a == std::wstring::npos || b == std::wstring::npos || c == std::wstring::npos) return {};
    auto path = L"SYSTEM\\CurrentControlSet\\Enum\\DISPLAY\\" + key.substr(a + 1, b - a - 1) + L"\\" + key.substr(b + 1, c - b - 1) + L"\\Device Parameters";
    BYTE bytes[4096]{}; DWORD size = sizeof(bytes);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, path.c_str(), L"EDID", RRF_RT_REG_BINARY, nullptr, bytes, &size) != ERROR_SUCCESS || size < 128) return {};
    const char* digits = "0123456789ABCDEF";
    std::string hex;
    for (size_t i = 0; i < 128; ++i) { hex += digits[bytes[i] >> 4]; hex += digits[bytes[i] & 15]; }
    return hex;
}
template<class T> bool supported(const T& ptr) {
    adlx_bool value = false;
    return ptr && ADLX_SUCCEEDED(ptr->IsSupported(&value)) && value;
}
class Amd final : public VendorDriver {
    Module dll{ADLX_DLL_NAMEW};
    ADLXTerminate_Fn terminate{};
    adlx::IADLXSystem* system{}; // Borrowed from ADLXInitialize; owned by ADLXTerminate.
    adlx::IADLXDisplayServicesPtr displays;
    adlx::IADLXDisplayPtr display;
    adlx::IADLXGPUPtr gpu;
    adlx::IADLXDisplayGPUScalingPtr gpuScale;
    adlx::IADLXDisplayScalingModePtr scaleMode;
    adlx::IADLXDisplayIntegerScalingPtr integer;
    adlx::IADLXDisplayFreeSyncPtr freeSync;
    adlx::IADLX3DWaitForVerticalRefreshPtr vsync;
    adlx::IADLX3DFrameRateTargetControlPtr limiter;
    DriverCapabilities caps;
    template<class T> void booleanEdit(T ptr, bool desired) {
        adlx_bool old = false; amdCheck(ptr->IsEnabled(&old));
        auto verify = [ptr](bool expected) { adlx_bool actual = false; amdCheck(ptr->IsEnabled(&actual)); if (actual != expected) throw std::runtime_error("AMD setting failed readback."); };
        edits.push_back({[ptr, desired] { amdCheck(ptr->SetEnabled(desired)); },
            [ptr, old, verify] { amdCheck(ptr->SetEnabled(old)); verify(old); }, [verify, desired] { verify(desired); }});
    }
    template<class T, class Value> void modeEdit(T ptr, Value desired) {
        Value old{}; amdCheck(ptr->GetMode(&old));
        auto verify = [ptr](Value expected) { Value actual{}; amdCheck(ptr->GetMode(&actual)); if (actual != expected) throw std::runtime_error("AMD mode failed readback."); };
        edits.push_back({[ptr, desired] { amdCheck(ptr->SetMode(desired)); },
            [ptr, old, verify] { amdCheck(ptr->SetMode(old)); verify(old); }, [verify, desired] { verify(desired); }});
    }
    void cleanup() {
        // Release all reference-counted interfaces BEFORE terminating the system.
        edits.clear(); limiter = nullptr; vsync = nullptr; freeSync = nullptr; integer = nullptr;
        scaleMode = nullptr; gpuScale = nullptr; gpu = nullptr; display = nullptr; displays = nullptr;
        if (system) { terminate(); system = nullptr; }
    }
public:
    explicit Amd(const Monitor& m) {
        terminate = dll.get<ADLXTerminate_Fn>(ADLX_TERMINATE_FUNCTION_NAME);
        amdCheck(dll.get<ADLXInitialize_Fn>(ADLX_INIT_FUNCTION_NAME)(ADLX_FULL_VERSION, &system));
        try {
            amdCheck(system->GetDisplaysServices(&displays));
            const auto wanted = windowsEdid(m.key);
            require(!wanted.empty(), "Unambiguous AMD monitor identification (EDID)");
            int windowsMatches = 0;
            for (auto& connected : connectedMonitors()) if (windowsEdid(connected.key) == wanted) ++windowsMatches;
            require(windowsMatches == 1, "Unambiguous AMD monitor identification (duplicate EDID)");
            adlx::IADLXDisplayListPtr list; amdCheck(displays->GetDisplays(&list));
            int matches = 0;
            for (adlx_uint i = list->Begin(); i != list->End(); ++i) {
                adlx::IADLXDisplayPtr candidate; amdCheck(list->At(i, &candidate));
                const char* edid = nullptr;
                if (!ADLX_SUCCEEDED(candidate->EDID(&edid)) || !edid || strlen(edid) < 256) continue;
                if (_strnicmp(edid, wanted.c_str(), 256) == 0) { display = candidate; ++matches; }
            }
            require(matches == 1, "Unambiguous AMD monitor identification");
            amdCheck(display->GetGPU(&gpu));
            displays->GetGPUScaling(display, &gpuScale); displays->GetScalingMode(display, &scaleMode);
            displays->GetIntegerScaling(display, &integer); displays->GetFreeSync(display, &freeSync);
            adlx::IADLX3DSettingsServicesPtr graphics;
            if (ADLX_SUCCEEDED(system->Get3DSettingsServices(&graphics)) && graphics) {
                graphics->GetWaitForVerticalRefresh(gpu, &vsync); graphics->GetFrameRateTargetControl(gpu, &limiter);
            }
            caps.scaling = supported(gpuScale) && supported(scaleMode);
            caps.integerScaling = supported(gpuScale) && supported(integer);
            caps.vrr = supported(freeSync); caps.vsync = supported(vsync); caps.fps = supported(limiter);
            if (caps.fps) {
                ADLX_IntRange range{};
                caps.fps = ADLX_SUCCEEDED(limiter->GetFPSRange(&range)) && range.minValue > 0 && range.maxValue >= range.minValue;
                if (caps.fps) { caps.minFps = range.minValue; caps.maxFps = std::min(1023, range.maxValue); caps.fpsStep = std::max(1, range.step); }
            }
            caps.notice = L"AMD ADLX: FreeSync is per-display; V-SYNC/FPS are GPU-wide. V-SYNC applies to OpenGL, not a universal Direct3D override.";
        } catch (...) { cleanup(); throw; }
    }
    ~Amd() override { cleanup(); }
    DriverCapabilities capabilities() override { return caps; }
    void capture(Preset& p) override {
        if (caps.scaling) {
            adlx_bool enabled = false; amdCheck(gpuScale->IsEnabled(&enabled));
            if (enabled) {
                ADLX_SCALE_MODE mode{}; amdCheck(scaleMode->GetMode(&mode));
                p.scaling = mode == CENTERED ? Scaling::none : mode == FULL_PANEL ? Scaling::stretch : Scaling::aspect;
                if (caps.integerScaling) { adlx_bool on = false; amdCheck(integer->IsEnabled(&on)); if (on) p.scaling = Scaling::integer; }
            }
        }
        if (caps.vrr) { adlx_bool on = false; amdCheck(freeSync->IsEnabled(&on)); p.gsync = on ? Gsync::fullscreen : Gsync::off; }
        if (caps.vsync) { ADLX_WAIT_FOR_VERTICAL_REFRESH_MODE mode{}; amdCheck(vsync->GetMode(&mode)); p.vsync = mode == WFVR_ALWAYS_OFF ? Vsync::off : mode == WFVR_ALWAYS_ON ? Vsync::on : Vsync::application; }
        if (caps.fps) { adlx_bool on = false; amdCheck(limiter->IsEnabled(&on)); adlx_int fps = 0; amdCheck(limiter->GetFPS(&fps)); p.maxFps = on ? fps : 0; }
    }
    void stage(const Preset& p) override {
        checkRequest(p, caps);
        if (p.scaling != Scaling::unchanged) {
            booleanEdit(gpuScale, true);
            if (supported(integer)) booleanEdit(integer, p.scaling == Scaling::integer);
            if (p.scaling != Scaling::integer) modeEdit(scaleMode, p.scaling == Scaling::none ? CENTERED : p.scaling == Scaling::stretch ? FULL_PANEL : PRESERVE_ASPECT_RATIO);
        }
        if (p.gsync != Gsync::unchanged) booleanEdit(freeSync, p.gsync != Gsync::off);
        if (p.vsync != Vsync::unchanged) modeEdit(vsync, p.vsync == Vsync::off ? WFVR_ALWAYS_OFF : p.vsync == Vsync::on ? WFVR_ALWAYS_ON : WFVR_OFF_UNLESS_APP_SPECIFIES);
        if (p.maxFps >= 0) {
            // Snapshot FPS even when disabling, so rollback retains its inactive value.
            adlx_int old = 0; amdCheck(limiter->GetFPS(&old));
            if (p.maxFps) {
                const int desired = p.maxFps;
                auto verify = [this](int expected) { adlx_int actual = 0; amdCheck(limiter->GetFPS(&actual)); if (actual != expected) throw std::runtime_error("AMD FPS failed readback."); };
                edits.push_back({[this, desired] { amdCheck(limiter->SetFPS(desired)); }, [this, old, verify] { amdCheck(limiter->SetFPS(old)); verify(old); }, [verify, desired] { verify(desired); }});
            }
            booleanEdit(limiter, p.maxFps != 0);
        }
    }
};
#endif
template<class T> T structure() { T t{}; t.Size = sizeof(t); return t; }
class Intel final : public VendorDriver {
    Module dll{L"ControlLib.dll"};
    ctl_api_handle_t api{};
    ctl_device_adapter_handle_t gpu{};
    ctl_display_output_handle_t display{};
    DriverCapabilities caps;
    uint64_t flipTypes = 0;
    bool retroSupported = false;
    template<class Fn, class... Args> auto call(const char* name, Args... args) { return dll.get<Fn>(name)(args...); }
    ctl_3d_feature_getset_t feature(ctl_3d_feature_t type, ctl_property_value_type_t valueType) {
        auto f = structure<ctl_3d_feature_getset_t>(); f.FeatureType = type; f.ValueType = valueType;
        char global[] = ""; f.ApplicationName = global;
        intelCheck(call<ctl_pfnGetSet3DFeature_t>("ctlGetSet3DFeature", gpu, &f));
        f.ApplicationName = nullptr; return f;
    }
    void writeFeature(ctl_3d_feature_getset_t f) {
        char global[] = ""; f.ApplicationName = global; f.bSet = true;
        intelCheck(call<ctl_pfnGetSet3DFeature_t>("ctlGetSet3DFeature", gpu, &f));
    }
    void featureEdit(ctl_3d_feature_getset_t old, ctl_3d_feature_getset_t desired) {
        auto verify = [this](ctl_3d_feature_getset_t expected) {
            auto actual = feature(expected.FeatureType, expected.ValueType);
            bool equal = expected.ValueType == CTL_PROPERTY_VALUE_TYPE_ENUM ? actual.Value.EnumType.EnableType == expected.Value.EnumType.EnableType :
                actual.Value.IntType.Enable == expected.Value.IntType.Enable && actual.Value.IntType.Value == expected.Value.IntType.Value;
            if (!equal) throw std::runtime_error("Intel 3D setting failed readback.");
        };
        edits.push_back({[this, desired] { writeFeature(desired); }, [this, old, verify] { writeFeature(old); verify(old); }, [verify, desired] { verify(desired); }});
    }
public:
    explicit Intel(const Monitor& m) {
        auto init = structure<ctl_init_args_t>(); init.AppVersion = CTL_IMPL_VERSION;
        intelCheck(call<ctl_pfnInit_t>("ctlInit", &init, &api));
        try {
            uint32_t count = 0;
            intelCheck(call<ctl_pfnEnumerateDevices_t>("ctlEnumerateDevices", api, &count, nullptr));
            require(count > 0 && count <= 64, "Intel adapter enumeration");
            std::vector<ctl_device_adapter_handle_t> devices(count);
            intelCheck(call<ctl_pfnEnumerateDevices_t>("ctlEnumerateDevices", api, &count, devices.data()));
            for (auto device : devices) {
                LUID luid{}; auto props = structure<ctl_device_adapter_properties_t>();
                props.pDeviceID = &luid; props.device_id_size = sizeof(luid);
                if (call<ctl_pfnGetDeviceProperties_t>("ctlGetDeviceProperties", device, &props) == CTL_RESULT_SUCCESS &&
                    luid.LowPart == m.adapterLow && luid.HighPart == m.adapterHigh) { gpu = device; break; }
            }
            require(gpu != nullptr, "Intel adapter matching");
            count = 0; intelCheck(call<ctl_pfnEnumerateDisplayOutputs_t>("ctlEnumerateDisplayOutputs", gpu, &count, nullptr));
            require(count > 0 && count <= 64, "Intel display enumeration");
            std::vector<ctl_display_output_handle_t> outputs(count);
            intelCheck(call<ctl_pfnEnumerateDisplayOutputs_t>("ctlEnumerateDisplayOutputs", gpu, &count, outputs.data()));
            for (auto output : outputs) {
                auto props = structure<ctl_display_properties_t>();
                if (call<ctl_pfnGetDisplayProperties_t>("ctlGetDisplayProperties", output, &props) == CTL_RESULT_SUCCESS &&
                    props.Os_display_encoder_handle.WindowsDisplayEncoderID == m.targetId) { display = output; break; }
            }
            require(display != nullptr, "Intel display matching");
            try {
                auto scale = structure<ctl_scaling_caps_t>();
                intelCheck(call<ctl_pfnGetSupportedScalingCapability_t>("ctlGetSupportedScalingCapability", display, &scale));
                dll.get<ctl_pfnSetCurrentScaling_t>("ctlSetCurrentScaling");
                const auto required = CTL_SCALING_TYPE_FLAG_CENTERED | CTL_SCALING_TYPE_FLAG_STRETCHED | CTL_SCALING_TYPE_FLAG_ASPECT_RATIO_CENTERED_MAX;
                caps.scaling = (scale.SupportedScaling & required) == required;
            } catch (const std::exception&) {}
            try {
                auto retro = structure<ctl_retro_scaling_caps_t>();
                intelCheck(call<ctl_pfnGetSupportedRetroScalingCapability_t>("ctlGetSupportedRetroScalingCapability", gpu, &retro));
                dll.get<ctl_pfnGetSetRetroScaling_t>("ctlGetSetRetroScaling");
                retroSupported = (retro.SupportedRetroScaling & CTL_RETRO_SCALING_TYPE_FLAG_INTEGER) != 0;
                caps.integerScaling = retroSupported;
            } catch (const std::exception&) {}
            try {
                auto info = structure<ctl_intel_arc_sync_monitor_params_t>();
                intelCheck(call<ctl_pfnGetIntelArcSyncInfoForMonitor_t>("ctlGetIntelArcSyncInfoForMonitor", display, &info));
                dll.get<ctl_pfnSetIntelArcSyncProfile_t>("ctlSetIntelArcSyncProfile");
                auto vrr = structure<ctl_intel_arc_sync_profile_params_t>();
                intelCheck(call<ctl_pfnGetIntelArcSyncProfile_t>("ctlGetIntelArcSyncProfile", display, &vrr));
                caps.vrr = info.IsIntelArcSyncSupported && vrr.IntelArcSyncProfile != CTL_INTEL_ARC_SYNC_PROFILE_INVALID;
            } catch (const std::exception&) {}
            try {
                auto features = structure<ctl_3d_feature_caps_t>();
                intelCheck(call<ctl_pfnGetSupported3DCapabilities_t>("ctlGetSupported3DCapabilities", gpu, &features));
                require(features.NumSupportedFeatures <= 256, "Intel capability enumeration");
                std::vector<ctl_3d_feature_details_t> details(features.NumSupportedFeatures);
                features.pFeatureDetails = details.data();
                intelCheck(call<ctl_pfnGetSupported3DCapabilities_t>("ctlGetSupported3DCapabilities", gpu, &features));
                for (auto& f : details) {
                    if (f.FeatureType == CTL_3D_FEATURE_GAMING_FLIP_MODES && f.ValueType == CTL_PROPERTY_VALUE_TYPE_ENUM) flipTypes = f.Value.EnumType.SupportedTypes;
                    if (f.FeatureType == CTL_3D_FEATURE_FRAME_LIMIT && f.ValueType == CTL_PROPERTY_VALUE_TYPE_INT32) {
                        auto r = f.Value.IntType.RangeInfo;
                        caps.fpsStep = std::max(1, r.step_size);
                        caps.minFps = r.min_possible_value == 0 ? caps.fpsStep : r.min_possible_value;
                        caps.maxFps = std::min(1023, r.max_possible_value);
                        caps.fps = caps.minFps > 0 && caps.maxFps >= caps.minFps;
                    }
                }
                // SupportedTypes is a bitmask of supported enum values (which are themselves flags for flip modes).
                caps.vsync = (flipTypes & CTL_GAMING_FLIP_MODE_FLAG_VSYNC_OFF) && (flipTypes & CTL_GAMING_FLIP_MODE_FLAG_VSYNC_ON);
            } catch (const std::exception&) {}
            if (caps.vsync) {
                try { feature(CTL_3D_FEATURE_GAMING_FLIP_MODES, CTL_PROPERTY_VALUE_TYPE_ENUM); }
                catch (const std::exception&) { caps.vsync = false; }
            }
            if (caps.fps) {
                try { feature(CTL_3D_FEATURE_FRAME_LIMIT, CTL_PROPERTY_VALUE_TYPE_INT32); }
                catch (const std::exception&) { caps.fps = false; }
            }
            caps.notice = L"Intel IGCL: Arc Sync is per-display. Integer scaling, gaming V-SYNC and FPS are GPU-wide; support depends on the driver/game API.";
        } catch (...) { call<ctl_pfnClose_t>("ctlClose", api); api = nullptr; throw; }
    }
    ~Intel() override { if (api) { try { call<ctl_pfnClose_t>("ctlClose", api); } catch (...) {} } }
    DriverCapabilities capabilities() override { return caps; }
    void capture(Preset& p) override {
        if (caps.scaling) {
            auto s = structure<ctl_scaling_settings_t>(); intelCheck(call<ctl_pfnGetCurrentScaling_t>("ctlGetCurrentScaling", display, &s));
            if (s.ScalingType == CTL_SCALING_TYPE_FLAG_CENTERED) p.scaling = Scaling::none;
            if (s.ScalingType == CTL_SCALING_TYPE_FLAG_STRETCHED) p.scaling = Scaling::stretch;
            if (s.ScalingType == CTL_SCALING_TYPE_FLAG_ASPECT_RATIO_CENTERED_MAX) p.scaling = Scaling::aspect;
        }
        if (retroSupported) { auto s = structure<ctl_retro_scaling_settings_t>(); s.Get = true; s.RetroScalingType = CTL_RETRO_SCALING_TYPE_FLAG_INTEGER; intelCheck(call<ctl_pfnGetSetRetroScaling_t>("ctlGetSetRetroScaling", gpu, &s)); if (s.Enable && s.RetroScalingType == CTL_RETRO_SCALING_TYPE_FLAG_INTEGER) p.scaling = Scaling::integer; }
        if (caps.vrr) { auto s = structure<ctl_intel_arc_sync_profile_params_t>(); intelCheck(call<ctl_pfnGetIntelArcSyncProfile_t>("ctlGetIntelArcSyncProfile", display, &s)); p.gsync = s.IntelArcSyncProfile == CTL_INTEL_ARC_SYNC_PROFILE_OFF ? Gsync::off : Gsync::fullscreen; }
        if (caps.vsync) { auto f = feature(CTL_3D_FEATURE_GAMING_FLIP_MODES, CTL_PROPERTY_VALUE_TYPE_ENUM); auto v = f.Value.EnumType.EnableType; if (v == 0) p.vsync = Vsync::application; if (v == CTL_GAMING_FLIP_MODE_FLAG_VSYNC_OFF) p.vsync = Vsync::off; if (v == CTL_GAMING_FLIP_MODE_FLAG_VSYNC_ON) p.vsync = Vsync::on; }
        if (caps.fps) { auto f = feature(CTL_3D_FEATURE_FRAME_LIMIT, CTL_PROPERTY_VALUE_TYPE_INT32); p.maxFps = f.Value.IntType.Enable ? f.Value.IntType.Value : 0; }
    }
    void stage(const Preset& p) override {
        checkRequest(p, caps);
        if (p.scaling != Scaling::unchanged) {
            if (retroSupported) {
                auto old = structure<ctl_retro_scaling_settings_t>(); old.Get = true; old.RetroScalingType = CTL_RETRO_SCALING_TYPE_FLAG_INTEGER;
                intelCheck(call<ctl_pfnGetSetRetroScaling_t>("ctlGetSetRetroScaling", gpu, &old));
                auto desired = old; desired.Enable = p.scaling == Scaling::integer; desired.RetroScalingType = CTL_RETRO_SCALING_TYPE_FLAG_INTEGER;
                auto write = [this](auto s) { s.Get = false; intelCheck(call<ctl_pfnGetSetRetroScaling_t>("ctlGetSetRetroScaling", gpu, &s)); };
                auto verify = [this](auto s) { auto a = structure<ctl_retro_scaling_settings_t>(); a.Get = true; a.RetroScalingType = CTL_RETRO_SCALING_TYPE_FLAG_INTEGER; intelCheck(call<ctl_pfnGetSetRetroScaling_t>("ctlGetSetRetroScaling", gpu, &a)); if (a.Enable != s.Enable || a.RetroScalingType != s.RetroScalingType) throw std::runtime_error("Intel integer scaling failed readback."); };
                edits.push_back({[write, desired] { write(desired); }, [write, verify, old] { write(old); verify(old); }, [verify, desired] { verify(desired); }});
            }
            if (p.scaling != Scaling::integer) {
                auto old = structure<ctl_scaling_settings_t>(); intelCheck(call<ctl_pfnGetCurrentScaling_t>("ctlGetCurrentScaling", display, &old));
                auto desired = old; desired.Enable = true; desired.HardwareModeSet = true;
                desired.ScalingType = p.scaling == Scaling::none ? CTL_SCALING_TYPE_FLAG_CENTERED : p.scaling == Scaling::stretch ? CTL_SCALING_TYPE_FLAG_STRETCHED : CTL_SCALING_TYPE_FLAG_ASPECT_RATIO_CENTERED_MAX;
                auto write = [this](auto s) { intelCheck(call<ctl_pfnSetCurrentScaling_t>("ctlSetCurrentScaling", display, &s)); };
                auto verify = [this](auto s) { auto a = structure<ctl_scaling_settings_t>(); intelCheck(call<ctl_pfnGetCurrentScaling_t>("ctlGetCurrentScaling", display, &a)); if (a.Enable != s.Enable || a.ScalingType != s.ScalingType || a.CustomScalingX != s.CustomScalingX || a.CustomScalingY != s.CustomScalingY) throw std::runtime_error("Intel scaling failed readback."); };
                edits.push_back({[write, desired] { write(desired); }, [write, verify, old] { write(old); verify(old); }, [verify, desired] { verify(desired); }});
            }
        }
        if (p.gsync != Gsync::unchanged) {
            auto old = structure<ctl_intel_arc_sync_profile_params_t>(); intelCheck(call<ctl_pfnGetIntelArcSyncProfile_t>("ctlGetIntelArcSyncProfile", display, &old));
            auto desired = old; desired.IntelArcSyncProfile = p.gsync == Gsync::off ? CTL_INTEL_ARC_SYNC_PROFILE_OFF : CTL_INTEL_ARC_SYNC_PROFILE_RECOMMENDED;
            auto write = [this](auto s) { intelCheck(call<ctl_pfnSetIntelArcSyncProfile_t>("ctlSetIntelArcSyncProfile", display, &s)); };
            auto verify = [this](auto s) { auto a = structure<ctl_intel_arc_sync_profile_params_t>(); intelCheck(call<ctl_pfnGetIntelArcSyncProfile_t>("ctlGetIntelArcSyncProfile", display, &a)); if (a.IntelArcSyncProfile != s.IntelArcSyncProfile || (s.IntelArcSyncProfile == CTL_INTEL_ARC_SYNC_PROFILE_CUSTOM && (a.MaxRefreshRateInHz != s.MaxRefreshRateInHz || a.MinRefreshRateInHz != s.MinRefreshRateInHz || a.MaxFrameTimeIncreaseInUs != s.MaxFrameTimeIncreaseInUs || a.MaxFrameTimeDecreaseInUs != s.MaxFrameTimeDecreaseInUs))) throw std::runtime_error("Intel Arc Sync failed readback."); };
            edits.push_back({[write, desired] { write(desired); }, [write, verify, old] { write(old); verify(old); }, [verify, desired] { verify(desired); }});
        }
        if (p.vsync != Vsync::unchanged) {
            auto old = feature(CTL_3D_FEATURE_GAMING_FLIP_MODES, CTL_PROPERTY_VALUE_TYPE_ENUM), desired = old;
            desired.Value.EnumType.EnableType = p.vsync == Vsync::off ? CTL_GAMING_FLIP_MODE_FLAG_VSYNC_OFF : p.vsync == Vsync::on ? CTL_GAMING_FLIP_MODE_FLAG_VSYNC_ON : 0;
            featureEdit(old, desired);
        }
        if (p.maxFps >= 0) {
            auto old = feature(CTL_3D_FEATURE_FRAME_LIMIT, CTL_PROPERTY_VALUE_TYPE_INT32), desired = old;
            desired.Value.IntType.Enable = p.maxFps != 0;
            if (p.maxFps) desired.Value.IntType.Value = p.maxFps;
            featureEdit(old, desired);
        }
    }
};
}
std::unique_ptr<VendorDriver> openVendorDriver(const Monitor& m) {
    if (m.vendor == Vendor::amd) {
#ifdef QS_ENABLE_AMD_ADLX
        return std::make_unique<Amd>(m);
#else
        return std::make_unique<Unavailable>(L"AMD driver controls require the optional ADLX build (disabled due to SDK licensing/EULA requirements). Basic display settings work without ADLX.");
#endif
    }
    if (m.vendor == Vendor::intel) return std::make_unique<Intel>(m);
    return std::make_unique<Unavailable>(m.vendor == Vendor::nvidia ?
        L"NVIDIA driver controls currently require all active displays to use NVIDIA. Basic display settings still work in mixed-GPU layouts." :
        L"No supported vendor driver API for this display. Resolution, refresh, primary monitor and Windows scaling still work.");
}
}
