// Public IGCL types, fake runtime entry points. Never loads a graphics driver or changes a display.
#include "vendor_driver.h"
#include <windows.h>
#include <igcl_api.h>
#include <iostream>
#include <functional>
#include <cstring>
namespace fake {
int failures = 0, writes = 0, opens = 0, closes = 0;
bool unsupported = false, wrongAdapter = false, failWrite = false, failUndo = false, ignoreWrite = false;
bool fixedRefresh = false;
uint32_t flip = CTL_GAMING_FLIP_MODE_FLAG_SMOOTH_SYNC;
bool limitEnabled = false, integerEnabled = false;
int limit = 75;
ctl_scaling_settings_t scaling{};
ctl_intel_arc_sync_profile_params_t vrr{};
void expect(bool value, const char* name) { if (!value) { ++failures; std::cerr << "FAIL: " << name << '\n'; } }
void rejects(const std::function<void()>& fn, const char* name) { try { fn(); expect(false, name); } catch (const std::exception&) {} }
void reset() {
    writes = opens = closes = 0; unsupported = wrongAdapter = failWrite = failUndo = ignoreWrite = false;
    fixedRefresh = false;
    flip = CTL_GAMING_FLIP_MODE_FLAG_SMOOTH_SYNC; limitEnabled = integerEnabled = false; limit = 75;
    scaling = {}; scaling.Size = sizeof(scaling); scaling.Enable = true; scaling.ScalingType = CTL_SCALING_TYPE_FLAG_CENTERED;
    vrr = {}; vrr.Size = sizeof(vrr); vrr.IntelArcSyncProfile = CTL_INTEL_ARC_SYNC_PROFILE_CUSTOM; vrr.MinRefreshRateInHz = 47; vrr.MaxRefreshRateInHz = 143;
}
ctl_device_adapter_handle_t gpu() { return reinterpret_cast<ctl_device_adapter_handle_t>(2); }
ctl_display_output_handle_t display() { return reinterpret_cast<ctl_display_output_handle_t>(3); }
ctl_result_t CTL_APICALL init(ctl_init_args_t*, ctl_api_handle_t* api) { ++opens; *api = reinterpret_cast<ctl_api_handle_t>(1); return CTL_RESULT_SUCCESS; }
ctl_result_t CTL_APICALL close(ctl_api_handle_t) { ++closes; return CTL_RESULT_SUCCESS; }
ctl_result_t CTL_APICALL enumerate(ctl_api_handle_t, uint32_t* count, ctl_device_adapter_handle_t* devices) { *count = 1; if (devices) *devices = gpu(); return CTL_RESULT_SUCCESS; }
ctl_result_t CTL_APICALL props(ctl_device_adapter_handle_t, ctl_device_adapter_properties_t* p) { *static_cast<LUID*>(p->pDeviceID) = {wrongAdapter ? 999u : 123u, 456}; return CTL_RESULT_SUCCESS; }
ctl_result_t CTL_APICALL outputs(ctl_device_adapter_handle_t, uint32_t* count, ctl_display_output_handle_t* devices) { *count = 1; if (devices) *devices = display(); return CTL_RESULT_SUCCESS; }
ctl_result_t CTL_APICALL displayProps(ctl_display_output_handle_t, ctl_display_properties_t* p) { p->Os_display_encoder_handle.WindowsDisplayEncoderID = 91; return CTL_RESULT_SUCCESS; }
ctl_result_t CTL_APICALL scaleCaps(ctl_display_output_handle_t, ctl_scaling_caps_t* p) { p->SupportedScaling = CTL_SCALING_TYPE_FLAG_CENTERED | CTL_SCALING_TYPE_FLAG_STRETCHED | CTL_SCALING_TYPE_FLAG_ASPECT_RATIO_CENTERED_MAX; return unsupported ? CTL_RESULT_ERROR_UNSUPPORTED_FEATURE : CTL_RESULT_SUCCESS; }
ctl_result_t CTL_APICALL retroCaps(ctl_device_adapter_handle_t, ctl_retro_scaling_caps_t* p) { p->SupportedRetroScaling = CTL_RETRO_SCALING_TYPE_FLAG_INTEGER; return unsupported ? CTL_RESULT_ERROR_UNSUPPORTED_FEATURE : CTL_RESULT_SUCCESS; }
ctl_result_t CTL_APICALL getScaling(ctl_display_output_handle_t, ctl_scaling_settings_t* p) { *p = scaling; return CTL_RESULT_SUCCESS; }
ctl_result_t CTL_APICALL setScaling(ctl_display_output_handle_t, ctl_scaling_settings_t* p) { ++writes; scaling = *p; return CTL_RESULT_SUCCESS; }
ctl_result_t CTL_APICALL retro(ctl_device_adapter_handle_t, ctl_retro_scaling_settings_t* p) {
    expect(p->RetroScalingType == CTL_RETRO_SCALING_TYPE_FLAG_INTEGER, "retro query specifies integer type");
    if (p->Get) p->Enable = integerEnabled;
    else { ++writes; integerEnabled = p->Enable; }
    return CTL_RESULT_SUCCESS;
}
ctl_result_t CTL_APICALL getVrr(ctl_display_output_handle_t, ctl_intel_arc_sync_profile_params_t* p) { *p = vrr; return unsupported ? CTL_RESULT_ERROR_UNSUPPORTED_FEATURE : CTL_RESULT_SUCCESS; }
ctl_result_t CTL_APICALL vrrInfo(ctl_display_output_handle_t, ctl_intel_arc_sync_monitor_params_t* p) { p->IsIntelArcSyncSupported = !fixedRefresh; return CTL_RESULT_SUCCESS; }
ctl_result_t CTL_APICALL setVrr(ctl_display_output_handle_t, ctl_intel_arc_sync_profile_params_t* p) { ++writes; vrr = *p; return CTL_RESULT_SUCCESS; }
ctl_result_t CTL_APICALL capabilities(ctl_device_adapter_handle_t, ctl_3d_feature_caps_t* p) {
    p->NumSupportedFeatures = 2;
    if (unsupported) return CTL_RESULT_ERROR_UNSUPPORTED_FEATURE;
    if (p->pFeatureDetails) {
        auto& f = p->pFeatureDetails[0]; f = {}; f.FeatureType = CTL_3D_FEATURE_GAMING_FLIP_MODES; f.ValueType = CTL_PROPERTY_VALUE_TYPE_ENUM;
        f.Value.EnumType.SupportedTypes = CTL_GAMING_FLIP_MODE_FLAG_VSYNC_OFF | CTL_GAMING_FLIP_MODE_FLAG_VSYNC_ON;
        auto& l = p->pFeatureDetails[1]; l = {}; l.FeatureType = CTL_3D_FEATURE_FRAME_LIMIT; l.ValueType = CTL_PROPERTY_VALUE_TYPE_INT32;
        l.Value.IntType.RangeInfo = {30, 240, 5, 60};
    }
    return CTL_RESULT_SUCCESS;
}
ctl_result_t CTL_APICALL feature(ctl_device_adapter_handle_t, ctl_3d_feature_getset_t* p) {
    expect(p->ApplicationName && !p->ApplicationName[0], "Intel edits target global policy, not a game profile");
    if (!p->bSet) {
        if (p->FeatureType == CTL_3D_FEATURE_FRAME_LIMIT) p->Value.IntType = {limitEnabled, limit};
        else p->Value.EnumType.EnableType = flip;
    } else {
        ++writes;
        if (failUndo && writes > 1) return CTL_RESULT_ERROR_DATA_WRITE;
        if (!ignoreWrite) {
            if (p->FeatureType == CTL_3D_FEATURE_FRAME_LIMIT) { limit = p->Value.IntType.Value; limitEnabled = p->Value.IntType.Enable; }
            else flip = p->Value.EnumType.EnableType;
        }
        if (failWrite) { failWrite = false; return CTL_RESULT_ERROR_DATA_WRITE; }
    }
    return CTL_RESULT_SUCCESS;
}
}
namespace qs {
FARPROC testVendorProc(const char* name) {
#define ENTRY(symbol, fn) if (strcmp(name, symbol) == 0) return reinterpret_cast<FARPROC>(fake::fn)
    ENTRY("ctlInit", init); ENTRY("ctlClose", close); ENTRY("ctlEnumerateDevices", enumerate); ENTRY("ctlGetDeviceProperties", props);
    ENTRY("ctlEnumerateDisplayOutputs", outputs); ENTRY("ctlGetDisplayProperties", displayProps);
    ENTRY("ctlGetSupportedScalingCapability", scaleCaps); ENTRY("ctlGetSupportedRetroScalingCapability", retroCaps);
    ENTRY("ctlGetCurrentScaling", getScaling); ENTRY("ctlSetCurrentScaling", setScaling); ENTRY("ctlGetSetRetroScaling", retro);
    ENTRY("ctlGetIntelArcSyncProfile", getVrr); ENTRY("ctlSetIntelArcSyncProfile", setVrr);
    ENTRY("ctlGetIntelArcSyncInfoForMonitor", vrrInfo);
    ENTRY("ctlGetSupported3DCapabilities", capabilities); ENTRY("ctlGetSet3DFeature", feature);
#undef ENTRY
    return nullptr;
}
}
int main() {
    using namespace qs; using namespace fake;
    Monitor m{L"lcd", L"LCD", L"DISPLAY1", true}; m.vendor = Vendor::intel; m.adapterLow = 123; m.adapterHigh = 456; m.targetId = 91;
    Preset p; p.name = L"Intel"; p.monitor = m.key; p.vsync = Vsync::on; p.maxFps = 120; p.gsync = Gsync::fullscreen; p.scaling = Scaling::aspect;
    try {
        reset(); {
            auto d = openVendorDriver(m); auto caps = d->capabilities();
            expect(caps.scaling && caps.integerScaling && caps.vrr && caps.vsync && caps.fps && caps.minFps == 30 && caps.maxFps == 240 && caps.fpsStep == 5, "runtime Intel capabilities/ranges");
            Preset captured; d->capture(captured);
            expect(captured.scaling == Scaling::none && captured.maxFps == 0 && captured.gsync == Gsync::fullscreen && writes == 0, "capture is read-only");
            d->stage(p); expect(writes == 0, "all driver snapshots staged before writes");
            d->apply(); expect(d->needsRestore() && flip == CTL_GAMING_FLIP_MODE_FLAG_VSYNC_ON && limit == 120 && limitEnabled && scaling.ScalingType == CTL_SCALING_TYPE_FLAG_ASPECT_RATIO_CENTERED_MAX, "Intel controls applied and verified");
            d->restore(); expect(!d->needsRestore() && flip == CTL_GAMING_FLIP_MODE_FLAG_SMOOTH_SYNC && limit == 75 && !limitEnabled && scaling.ScalingType == CTL_SCALING_TYPE_FLAG_CENTERED && vrr.IntelArcSyncProfile == CTL_INTEL_ARC_SYNC_PROFILE_CUSTOM && vrr.MinRefreshRateInHz == 47, "restore exact raw values including custom VRR and disabled FPS");
        } expect(opens == closes, "SDK close paired with initialization");
        reset(); { auto d = openVendorDriver(m); auto bad = p; bad.maxFps = 121; rejects([&] { d->stage(bad); }, "out-of-step FPS rejected"); expect(writes == 0, "invalid FPS causes no writes"); bad = p; bad.gsync = Gsync::windowed; rejects([&] { d->stage(bad); }, "NVIDIA-specific VRR policy rejected on Intel"); }
        reset(); unsupported = true; { auto d = openVendorDriver(m); auto caps = d->capabilities(); expect(!caps.scaling && !caps.integerScaling && !caps.vrr && !caps.vsync && !caps.fps, "unsupported Intel features hidden"); rejects([&] { d->stage(p); }, "old preset unsupported values are not silently skipped"); expect(writes == 0, "unsupported request causes no writes"); }
        reset(); wrongAdapter = true; rejects([&] { openVendorDriver(m); }, "never uses another Intel GPU"); expect(opens == closes, "constructor failure closes SDK");
        reset(); fixedRefresh = true; { auto d = openVendorDriver(m); expect(!d->capabilities().vrr && d->capabilities().fps, "fixed-refresh monitors hide VRR even if profile read succeeds"); }
        reset(); { auto d = openVendorDriver(m); Preset one; one.vsync = Vsync::off; d->stage(one); failWrite = true; rejects([&] { d->apply(); }, "partial vendor write failure surfaced"); expect(d->needsRestore(), "partial failure retains undo"); d->restore(); expect(flip == CTL_GAMING_FLIP_MODE_FLAG_SMOOTH_SYNC, "partial failure can restore"); }
        reset(); { auto d = openVendorDriver(m); Preset one; one.maxFps = 0; limitEnabled = true; d->stage(one); d->apply(); expect(!limitEnabled && limit == 75, "FPS zero disables without destroying inactive cap"); failUndo = true; rejects([&] { d->restore(); }, "failed undo surfaced"); expect(d->needsRestore(), "failed undo remains retryable"); failUndo = false; d->restore(); expect(limitEnabled && !d->needsRestore(), "failed undo retry"); }
        reset(); { auto d = openVendorDriver(m); Preset one; one.vsync = Vsync::on; d->stage(one); ignoreWrite = true; rejects([&] { d->apply(); }, "readback rejects ignored driver writes"); ignoreWrite = false; d->restore(); }
        reset(); { m.vendor = Vendor::amd; auto d = openVendorDriver(m); expect(!d->capabilities().fps, "default AMD build explains unavailable ADLX controls"); Preset basic; d->stage(basic); d->apply(); expect(writes == 0, "basic AMD preset needs no driver API"); rejects([&] { d->stage(p); }, "AMD API-specific preset blocked without opt-in SDK"); }
    } catch (const std::exception& e) { ++failures; std::cerr << "Unexpected: " << e.what() << '\n'; }
    if (!failures) std::cout << "Vendor transactions passed using fake IGCL only. No driver/display changes.\n";
    return failures ? 1 : 0;
}
