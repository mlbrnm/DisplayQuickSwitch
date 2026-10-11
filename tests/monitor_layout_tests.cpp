// Production layout engine linked against fake Windows CCD entry points. No real monitor APIs.
#include "monitor_layout.h"
#include "layout_test_api.h"
#include <array>
#include <iostream>
#include <functional>
#include <algorithm>

namespace fake {
struct Display {
    bool connected = true, enabled = true;
    UINT32 source = 0;
    DISPLAYCONFIG_SOURCE_MODE mode{};
    DISPLAYCONFIG_RATIONAL refresh{120000, 1000};
};
std::array<Display, 3> displays;
int failures = 0, writes = 0, queries = 0;
bool noDatabase = false, changeDatabaseModes = false, failTopology = false, failFinal = false;
bool failRollback = false, rollbackNeeded = false, ignoreTopology = false, clone = false;
bool restrictedRoutes = false;
bool onlyOneSource = false;
void expect(bool value, const char* label) { if (!value) { std::cerr << "FAIL: " << label << '\n'; ++failures; } }
void rejects(const std::function<void()>& fn, const char* label) { try { fn(); expect(false, label); } catch (const std::exception&) {} }
void reset() {
    displays = {}; writes = queries = 0;
    noDatabase = changeDatabaseModes = failTopology = failFinal = failRollback = rollbackNeeded = ignoreTopology = clone = false;
    restrictedRoutes = false;
    onlyOneSource = false;
    for (UINT32 i = 0; i < 3; ++i) { displays[i].connected = displays[i].enabled = true; displays[i].source = i; displays[i].mode.pixelFormat = DISPLAYCONFIG_PIXELFORMAT_32BPP; }
    displays[0].mode.width = 3840; displays[0].mode.height = 2160;
    displays[1].mode.width = 1600; displays[1].mode.height = 1200; displays[1].mode.position = {3840, 0}; displays[1].refresh = {85000, 1000};
    displays[2].mode.width = 2560; displays[2].mode.height = 1440; displays[2].mode.position = {5440, 0}; displays[2].connected = displays[2].enabled = false;
}
const wchar_t* key(size_t i) { return i == 0 ? L"lcd" : i == 1 ? L"crt" : L"extra"; }
struct State { std::vector<DISPLAYCONFIG_PATH_INFO> paths; std::vector<DISPLAYCONFIG_MODE_INFO> modes; };
State snapshot(UINT32 flags) {
    State state;
    for (UINT32 i = 0; i < 3; ++i) {
        const auto& d = displays[i];
        if (!d.connected) continue;
        auto add = [&](UINT32 source, bool active) {
            DISPLAYCONFIG_PATH_INFO p{}; p.sourceInfo.adapterId = {123, 456}; p.sourceInfo.id = source;
            p.targetInfo.adapterId = {123, 456}; p.targetInfo.id = 91 + i; p.targetInfo.targetAvailable = TRUE;
            p.targetInfo.refreshRate = d.refresh; p.targetInfo.rotation = DISPLAYCONFIG_ROTATION_IDENTITY; p.targetInfo.scaling = DISPLAYCONFIG_SCALING_IDENTITY;
            p.sourceInfo.modeInfoIdx = p.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
            if (active) {
                p.flags = DISPLAYCONFIG_PATH_ACTIVE;
                DISPLAYCONFIG_MODE_INFO m{}; m.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE; m.id = source; m.adapterId = p.sourceInfo.adapterId; m.sourceMode = d.mode;
                p.sourceInfo.modeInfoIdx = static_cast<UINT32>(state.modes.size()); state.modes.push_back(m);
                m = {}; m.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_TARGET; m.id = p.targetInfo.id; m.adapterId = p.targetInfo.adapterId;
                m.targetMode.targetVideoSignalInfo.vSyncFreq = d.refresh;
                p.targetInfo.modeInfoIdx = static_cast<UINT32>(state.modes.size()); state.modes.push_back(m);
            }
            state.paths.push_back(p);
        };
        if (flags == QDC_ONLY_ACTIVE_PATHS) { if (d.enabled) add(clone ? 0 : d.source, true); }
        else for (UINT32 source = 0; source < 3; ++source) {
            if (onlyOneSource && source > 0) continue;
            if (restrictedRoutes && ((i == 0 && source > 1) || (i == 1 && source > 0))) continue;
            add(source, d.enabled && (clone ? source == 0 : source == d.source));
        }
    }
    return state;
}
void original(const char* label) {
    expect(displays[0].enabled && displays[1].enabled && !displays[2].enabled &&
        displays[0].mode.position.x == 0 && displays[1].mode.position.x == 3840 &&
        displays[0].mode.width == 3840 && displays[1].mode.width == 1600, label);
}
}
namespace qs::testLayout {
LONG bufferSizes(UINT32 flags, UINT32* paths, UINT32* modes) {
    auto state = fake::snapshot(flags); *paths = static_cast<UINT32>(state.paths.size()); *modes = static_cast<UINT32>(state.modes.size()); return ERROR_SUCCESS;
}
LONG query(UINT32 flags, UINT32* paths, DISPLAYCONFIG_PATH_INFO* p, UINT32* modes, DISPLAYCONFIG_MODE_INFO* m, DISPLAYCONFIG_TOPOLOGY_ID*) {
    ++fake::queries;
    auto state = fake::snapshot(flags);
    if (*paths < state.paths.size() || *modes < state.modes.size()) return ERROR_INSUFFICIENT_BUFFER;
    std::copy(state.paths.begin(), state.paths.end(), p); std::copy(state.modes.begin(), state.modes.end(), m);
    *paths = static_cast<UINT32>(state.paths.size()); *modes = static_cast<UINT32>(state.modes.size()); return ERROR_SUCCESS;
}
LONG getDevice(DISPLAYCONFIG_DEVICE_INFO_HEADER* header) {
    if (header->type == DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME) {
        auto* p = reinterpret_cast<DISPLAYCONFIG_TARGET_DEVICE_NAME*>(header);
        wcscpy_s(p->monitorDevicePath, fake::key(header->id - 91)); wcscpy_s(p->monitorFriendlyDeviceName, fake::key(header->id - 91));
    } else if (header->type == DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME) {
        auto* p = reinterpret_cast<DISPLAYCONFIG_SOURCE_DEVICE_NAME*>(header); wcscpy_s(p->viewGdiDeviceName, L"DISPLAY");
    } else return ERROR_INVALID_PARAMETER;
    return ERROR_SUCCESS;
}
LONG setConfig(UINT32 count, DISPLAYCONFIG_PATH_INFO* paths, UINT32 modeCount, DISPLAYCONFIG_MODE_INFO* modes, UINT32 flags) {
    fake::expect(count > 0, "never disables every monitor");
    fake::expect(!((flags & SDC_TOPOLOGY_SUPPLIED) && (flags & SDC_SAVE_TO_DATABASE)), "database flag combination is valid");
    if (flags & SDC_VALIDATE) return fake::noDatabase && (flags & SDC_TOPOLOGY_SUPPLIED) ? ERROR_BAD_CONFIGURATION : ERROR_SUCCESS;
    ++fake::writes;
    if (fake::rollbackNeeded && fake::failRollback) return ERROR_GEN_FAILURE;
    if (fake::ignoreTopology && modeCount == 0) { fake::ignoreTopology = false; return ERROR_SUCCESS; }
    for (auto& d : fake::displays) d.enabled = false;
    for (UINT32 i = 0; i < count; ++i) {
        auto& p = paths[i]; auto& d = fake::displays[p.targetInfo.id - 91];
        d.enabled = true; d.source = p.sourceInfo.id;
        if (p.sourceInfo.modeInfoIdx < modeCount) d.mode = modes[p.sourceInfo.modeInfoIdx].sourceMode;
        else if (fake::changeDatabaseModes && d.mode.width == 3840) { d.mode.width = 1024; d.mode.height = 768; }
        d.refresh = p.targetInfo.refreshRate;
    }
    if ((modeCount == 0 && fake::failTopology) || (modeCount != 0 && fake::failFinal && !fake::rollbackNeeded)) {
        fake::failTopology = fake::failFinal = false; fake::rollbackNeeded = true; return ERROR_GEN_FAILURE;
    }
    return ERROR_SUCCESS;
}
}
int main() {
    using namespace qs; using namespace fake;
    try {
        reset(); displays[1].enabled = false;
        auto connected = connectedMonitors();
        expect(connected.size() == 2 && connected[0].enabled && !connected[1].enabled, "connected list includes disabled targets and deduplicates routes");
        LayoutPreset both{L"Both, CRT primary", L"crt", {{L"lcd", true}, {L"crt", true}}};
        MonitorLayoutTransaction enabled; enabled.apply(both);
        expect(displays[0].enabled && displays[1].enabled && displays[1].mode.position.x == 0 && displays[0].mode.position.x == -3840, "enable disabled monitor and set its primary");
        enabled.restore(); expect(displays[0].enabled && !displays[1].enabled && displays[0].mode.position.x == 0, "undo restores disabled set and primary");
        reset(); LayoutPreset crtOnly{L"CRT only", L"crt", {{L"lcd", false}, {L"crt", true}}};
        MonitorLayoutTransaction disabled; disabled.apply(crtOnly);
        expect(!displays[0].enabled && displays[1].enabled && displays[1].mode.position.x == 0, "disable previous primary safely");
        disabled.restore(); original("restore entire previous layout");
        reset(); displays[1].enabled = false; changeDatabaseModes = true;
        MonitorLayoutTransaction preserve; preserve.apply(both);
        expect(displays[0].mode.width == 3840 && displays[0].mode.height == 2160, "restore retained monitors' modes after Windows database lookup");
        preserve.restore();
        reset();
        MonitorLayoutTransaction primaryOnly; primaryOnly.apply(both);
        expect(writes == 1 && displays[1].mode.position.x == 0, "primary-only layouts need only one modeset"); primaryOnly.restore();
        reset(); displays[1].enabled = false; restrictedRoutes = true;
        MonitorLayoutTransaction reroute; reroute.apply(both);
        expect(displays[0].source == 1 && displays[1].source == 0 && displays[0].mode.width == 3840, "source matching reroutes retained monitor when needed to enable another"); reroute.restore();
        reset(); displays[1].enabled = false; onlyOneSource = true;
        MonitorLayoutTransaction impossible; rejects([&] { impossible.apply(both); }, "impossible independent source assignment is rejected");
        expect(writes == 0, "impossible routing rejected before any changes");
        reset(); noDatabase = true;
        MonitorLayoutTransaction fallback; fallback.apply(crtOnly); expect(!displays[0].enabled && displays[1].enabled, "best-mode fallback for unsaved monitor combinations"); fallback.restore();
        reset(); auto missing = both; missing.monitors.push_back({L"missing", true});
        MonitorLayoutTransaction absent; rejects([&] { absent.apply(missing); }, "missing enabled monitor rejected"); expect(writes == 0, "missing enabled target causes no writes");
        reset(); displays[1].connected = displays[1].enabled = false;
        LayoutPreset lcdOnly{L"LCD only", L"lcd", {{L"lcd", true}, {L"crt", false}}};
        MonitorLayoutTransaction missingDisabled; missingDisabled.apply(lcdOnly); expect(displays[0].enabled, "missing disabled monitor is harmless"); missingDisabled.restore();
        reset(); displays[2].connected = displays[2].enabled = true;
        MonitorLayoutTransaction extra; extra.apply(lcdOnly); expect(displays[2].enabled && !displays[1].enabled, "unlisted new monitor is left enabled rather than unexpectedly disabled"); extra.restore();
        reset(); failTopology = true;
        MonitorLayoutTransaction partial; rejects([&] { partial.apply(crtOnly); }, "partial topology failure reported"); original("partial topology failure rolls back"); expect(!partial.needsRestore(), "successful rollback clears dirty state");
        reset(); failFinal = true;
        MonitorLayoutTransaction finalFailure; rejects([&] { finalFailure.apply(crtOnly); }, "primary/final write failure reported"); original("final write failure rolls back");
        reset(); displays[1].enabled = false; ignoreTopology = true;
        MonitorLayoutTransaction ignored; rejects([&] { ignored.apply(both); }, "enabled-set readback mismatch reported"); expect(displays[0].enabled && !displays[1].enabled, "ignored apply restores original monitor set");
        reset(); failTopology = failRollback = true;
        MonitorLayoutTransaction retry; rejects([&] { retry.apply(crtOnly); }, "failed rollback reported"); expect(retry.needsRestore(), "failed rollback retains snapshot"); failRollback = false; retry.restore(); original("rollback can be retried");
        reset(); clone = true;
        MonitorLayoutTransaction cloned; rejects([&] { cloned.apply(both); }, "cloned input rejected before changes"); expect(writes == 0, "clone rejection causes no writes");
        reset(); auto unsafe = both; for (auto& m : unsafe.monitors) m.enabled = false;
        MonitorLayoutTransaction empty; rejects([&] { empty.apply(unsafe); }, "zero enabled monitors rejected"); expect(writes == 0 && queries == 0, "unsafe layout rejected before APIs");
    } catch (const std::exception& e) { std::cerr << "Unexpected: " << e.what() << '\n'; ++failures; }
    if (!failures) std::cout << "Monitor layout tests passed with fake CCD only. No real display changes.\n";
    return failures ? 1 : 0;
}
