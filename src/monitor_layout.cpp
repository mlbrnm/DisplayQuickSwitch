#include "monitor_layout.h"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <thread>
#include <limits>
#ifdef QS_TEST_LAYOUT
#include "layout_test_api.h"
#define GetDisplayConfigBufferSizes qs::testLayout::bufferSizes
#define QueryDisplayConfig qs::testLayout::query
#define DisplayConfigGetDeviceInfo qs::testLayout::getDevice
#define SetDisplayConfig qs::testLayout::setConfig
#endif

namespace qs {
namespace {
void checkWindows(LONG status, const char* action) {
    if (status != ERROR_SUCCESS) throw std::runtime_error(std::string(action) + " (Windows error " + std::to_string(status) + ").");
}
bool same(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }
bool sameAdapter(const LUID& a, const LUID& b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }
bool sameSource(const DISPLAYCONFIG_PATH_INFO& a, const DISPLAYCONFIG_PATH_INFO& b) {
    return a.sourceInfo.id == b.sourceInfo.id && sameAdapter(a.sourceInfo.adapterId, b.sourceInfo.adapterId);
}
struct NamedPath { DISPLAYCONFIG_PATH_INFO path; Monitor monitor; };
struct State {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    std::vector<NamedPath> named;
    static State read(UINT32 flags) {
        State state;
        UINT32 paths = 0, modes = 0;
        LONG status = ERROR_INSUFFICIENT_BUFFER;
        for (int attempt = 0; attempt < 5 && status == ERROR_INSUFFICIENT_BUFFER; ++attempt) {
            checkWindows(GetDisplayConfigBufferSizes(flags, &paths, &modes), "Count Windows display paths");
            if (paths > 4096 || modes > 16384) throw std::runtime_error("Unexpected Windows display configuration size.");
            state.paths.resize(paths); state.modes.resize(modes);
            status = QueryDisplayConfig(flags, &paths, state.paths.data(), &modes, state.modes.data(), nullptr);
        }
        checkWindows(status, "Read Windows display configuration");
        state.paths.resize(paths); state.modes.resize(modes);
        for (const auto& p : state.paths) {
            bool active = (p.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;
            if (!active && !p.targetInfo.targetAvailable) continue;
            DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
            target.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof(target), p.targetInfo.adapterId, p.targetInfo.id};
            auto result = DisplayConfigGetDeviceInfo(&target.header);
            if (!active && (result != ERROR_SUCCESS || !target.monitorDevicePath[0])) continue;
            checkWindows(result, "Identify monitor for layout");
            if (!target.monitorDevicePath[0]) throw std::runtime_error("An active monitor has no stable identity.");
            Monitor monitor{target.monitorDevicePath, target.monitorFriendlyDeviceName, L"", false, active};
            if (monitor.name.empty()) monitor.name = L"Monitor";
            if (active) {
                const auto& source = state.source(p);
                monitor.primary = source.position.x == 0 && source.position.y == 0;
                DISPLAYCONFIG_SOURCE_DEVICE_NAME name{};
                name.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(name), p.sourceInfo.adapterId, p.sourceInfo.id};
                if (DisplayConfigGetDeviceInfo(&name.header) == ERROR_SUCCESS) monitor.gdiName = name.viewGdiDeviceName;
            }
            state.named.push_back({p, std::move(monitor)});
        }
        return state;
    }
    const DISPLAYCONFIG_SOURCE_MODE& source(const DISPLAYCONFIG_PATH_INFO& path) const {
        auto index = path.sourceInfo.modeInfoIdx;
        if (index >= modes.size() || modes[index].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE ||
            modes[index].id != path.sourceInfo.id || !sameAdapter(modes[index].adapterId, path.sourceInfo.adapterId))
            throw std::runtime_error("Windows returned invalid source mode information.");
        return modes[index].sourceMode;
    }
    const NamedPath& find(const std::wstring& key) const {
        for (const auto& entry : named) if (same(entry.monitor.key, key)) return entry;
        throw std::runtime_error("An enabled/primary monitor in this layout is disconnected or no longer available.");
    }
    void requireExtended() const {
        if (named.empty()) throw std::runtime_error("No active monitors were found.");
        for (size_t i = 0; i < named.size(); ++i)
            for (size_t j = 0; j < i; ++j) if (sameSource(named[i].path, named[j].path))
                throw std::runtime_error("Monitor layouts require extended displays, not clone mode.");
    }
    void write(UINT32 flags) {
        checkWindows(SetDisplayConfig(static_cast<UINT32>(paths.size()), paths.data(), static_cast<UINT32>(modes.size()),
            modes.empty() ? nullptr : modes.data(), flags), "Apply/validate monitor layout");
    }
};
template<class Check> State settled(Check&& verify) {
    for (int attempt = 0; ; ++attempt) {
        try { auto state = State::read(QDC_ONLY_ACTIVE_PATHS); state.requireExtended(); verify(state); return state; }
        catch (...) { if (attempt == 9) throw; std::this_thread::sleep_for(std::chrono::milliseconds(200)); }
    }
}
bool contains(const std::vector<std::wstring>& keys, const std::wstring& key) {
    return std::any_of(keys.begin(), keys.end(), [&](const auto& k) { return same(k, key); });
}
void verifyEnabled(const State& state, const std::vector<std::wstring>& wanted) {
    if (state.named.size() != wanted.size()) throw std::runtime_error("Windows did not accept the enabled monitor set.");
    for (auto& key : wanted) state.find(key);
}
double hz(const DISPLAYCONFIG_RATIONAL& value) { return value.Denominator ? static_cast<double>(value.Numerator) / value.Denominator : 0; }
void verifySame(const State& current, const State& expected, bool positions) {
    if (current.named.size() != expected.named.size()) throw std::runtime_error("Previous monitor set did not pass readback.");
    for (const auto& before : expected.named) {
        const auto& after = current.find(before.monitor.key);
        const auto& a = current.source(after.path); const auto& b = expected.source(before.path);
        if (a.width != b.width || a.height != b.height || a.pixelFormat != b.pixelFormat ||
            (positions && (a.position.x != b.position.x || a.position.y != b.position.y)) ||
            after.path.targetInfo.rotation != before.path.targetInfo.rotation ||
            std::abs(hz(after.path.targetInfo.refreshRate) - hz(before.path.targetInfo.refreshRate)) > 0.5)
            throw std::runtime_error("Monitor mode/layout did not pass readback verification.");
    }
}
void restoreRetainedModes(State& current, const State& before) {
    for (auto& entry : current.named) {
        auto old = std::find_if(before.named.begin(), before.named.end(), [&](auto& p) { return same(p.monitor.key, entry.monitor.key); });
        if (old == before.named.end()) continue;
        const auto& source = before.source(old->path);
        current.modes[entry.path.sourceInfo.modeInfoIdx].sourceMode = source; // Preserve previous mode AND position.
        auto& path = *std::find_if(current.paths.begin(), current.paths.end(), [&](auto& p) {
            return p.targetInfo.id == entry.path.targetInfo.id && sameAdapter(p.targetInfo.adapterId, entry.path.targetInfo.adapterId);
        });
        path.targetInfo.rotation = old->path.targetInfo.rotation;
        path.targetInfo.scaling = old->path.targetInfo.scaling;
        path.targetInfo.refreshRate = old->path.targetInfo.refreshRate;
        path.targetInfo.scanLineOrdering = old->path.targetInfo.scanLineOrdering;
        if (old->path.targetInfo.modeInfoIdx < before.modes.size() && path.targetInfo.modeInfoIdx < current.modes.size())
            current.modes[path.targetInfo.modeInfoIdx].targetMode = before.modes[old->path.targetInfo.modeInfoIdx].targetMode;
        entry.path = path;
    }
}
bool overlaps(const DISPLAYCONFIG_SOURCE_MODE& a, const DISPLAYCONFIG_SOURCE_MODE& b) {
    return static_cast<int64_t>(a.position.x) < static_cast<int64_t>(b.position.x) + b.width &&
        static_cast<int64_t>(b.position.x) < static_cast<int64_t>(a.position.x) + a.width &&
        static_cast<int64_t>(a.position.y) < static_cast<int64_t>(b.position.y) + b.height &&
        static_cast<int64_t>(b.position.y) < static_cast<int64_t>(a.position.y) + a.height;
}
void placeAndRebase(State& current, const State& before, const std::wstring& primary) {
    std::vector<DISPLAYCONFIG_SOURCE_MODE*> placed;
    for (auto& p : current.named)
        if (std::any_of(before.named.begin(), before.named.end(), [&](auto& old) { return same(old.monitor.key, p.monitor.key); }))
            placed.push_back(&current.modes[p.path.sourceInfo.modeInfoIdx].sourceMode);
    for (auto& p : current.named) {
        auto* source = &current.modes[p.path.sourceInfo.modeInfoIdx].sourceMode;
        if (std::find(placed.begin(), placed.end(), source) != placed.end()) continue;
        if (std::any_of(placed.begin(), placed.end(), [&](auto* other) { return overlaps(*source, *other); })) {
            int64_t right = 0;
            for (auto* other : placed) right = std::max(right, static_cast<int64_t>(other->position.x) + other->width);
            if (right > INT32_MAX) throw std::runtime_error("Monitor layout position overflow.");
            source->position = {static_cast<LONG>(right), 0};
        }
        placed.push_back(source);
    }
    std::vector<Position> positions;
    size_t selected = current.named.size();
    for (size_t i = 0; i < current.named.size(); ++i) {
        auto& source = current.source(current.named[i].path);
        positions.push_back({source.position.x, source.position.y});
        if (same(current.named[i].monitor.key, primary)) selected = i;
    }
    auto rebased = rebasePositions(positions, selected);
    for (size_t i = 0; i < current.named.size(); ++i)
        current.modes[current.named[i].path.sourceInfo.modeInfoIdx].sourceMode.position = {rebased[i].x, rebased[i].y};
}
}

std::vector<Monitor> connectedMonitors() {
    auto state = State::read(QDC_ALL_PATHS);
    std::vector<Monitor> result;
    for (const auto& entry : state.named) {
        auto existing = std::find_if(result.begin(), result.end(), [&](auto& m) { return same(m.key, entry.monitor.key); });
        if (existing == result.end()) result.push_back(entry.monitor);
        else if (entry.monitor.enabled) *existing = entry.monitor;
    }
    return result;
}
struct MonitorLayoutTransaction::Impl { State before; bool dirty = false; };
MonitorLayoutTransaction::MonitorLayoutTransaction() : impl(std::make_unique<Impl>()) {}
MonitorLayoutTransaction::~MonitorLayoutTransaction() = default;
bool MonitorLayoutTransaction::needsRestore() const { return impl->dirty; }
void MonitorLayoutTransaction::apply(const LayoutPreset& preset) {
    validate(preset);
    if (impl->dirty) throw std::runtime_error("Layout transaction already contains applied settings.");
    impl->before = State::read(QDC_ONLY_ACTIVE_PATHS); impl->before.requireExtended();
    const auto all = State::read(QDC_ALL_PATHS);
    std::vector<std::wstring> wanted;
    for (const auto& entry : all.named) {
        auto specified = std::find_if(preset.monitors.begin(), preset.monitors.end(), [&](auto& m) { return same(m.monitor, entry.monitor.key); });
        bool enable = specified == preset.monitors.end() ? entry.monitor.enabled : specified->enabled;
        if (enable && !contains(wanted, entry.monitor.key)) wanted.push_back(entry.monitor.key);
    }
    for (const auto& m : preset.monitors) if (m.enabled) all.find(m.monitor); // Missing enabled target is an error, not a partial preset.
    if (!contains(wanted, preset.primaryMonitor)) throw std::runtime_error("Primary layout monitor is unavailable.");
    // Prefer active routes, but permit reassignment if a newly enabled monitor needs
    // that source. Bipartite matching avoids a greedy choice rejecting a valid set.
    std::stable_sort(wanted.begin(), wanted.end(), [&](auto& a, auto& b) {
        bool aa = std::any_of(impl->before.named.begin(), impl->before.named.end(), [&](auto& p) { return same(p.monitor.key, a); });
        bool bb = std::any_of(impl->before.named.begin(), impl->before.named.end(), [&](auto& p) { return same(p.monitor.key, b); });
        return aa && !bb;
    });
    State topology;
    std::vector<std::vector<const NamedPath*>> candidates(wanted.size());
    std::vector<const NamedPath*> chosen(wanted.size(), nullptr);
    for (size_t i = 0; i < wanted.size(); ++i) {
        for (const auto& candidate : all.named) if (same(candidate.monitor.key, wanted[i])) candidates[i].push_back(&candidate);
        std::stable_sort(candidates[i].begin(), candidates[i].end(), [](auto* a, auto* b) { return a->monitor.enabled && !b->monitor.enabled; });
    }
    auto assign = [&](auto&& self, size_t index, std::vector<bool>& visited) -> bool {
        if (visited[index]) return false;
        visited[index] = true;
        for (auto* candidate : candidates[index]) {
            size_t owner = chosen.size();
            for (size_t i = 0; i < chosen.size(); ++i) if (chosen[i] && sameSource(chosen[i]->path, candidate->path)) { owner = i; break; }
            if (owner == chosen.size() || (owner != index && self(self, owner, visited))) { chosen[index] = candidate; return true; }
        }
        return false;
    };
    for (size_t i = 0; i < wanted.size(); ++i) {
        std::vector<bool> visited(wanted.size(), false);
        if (!assign(assign, i, visited)) throw std::runtime_error("Windows has no independent display sources for the requested monitor set.");
    }
    for (auto* candidate : chosen) {
        auto path = candidate->path;
        path.flags |= DISPLAYCONFIG_PATH_ACTIVE;
        path.sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        path.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        topology.paths.push_back(path);
    }
    constexpr UINT32 database = SDC_TOPOLOGY_SUPPLIED | SDC_ALLOW_PATH_ORDER_CHANGES;
    UINT32 topologyFlags = database;
    const bool unchangedSet = wanted.size() == impl->before.named.size() &&
        std::all_of(impl->before.named.begin(), impl->before.named.end(), [&](auto& m) { return contains(wanted, m.monitor.key); });
    if (!unchangedSet) {
        auto status = SetDisplayConfig(static_cast<UINT32>(topology.paths.size()), topology.paths.data(), 0, nullptr, database | SDC_VALIDATE);
        if (status == ERROR_BAD_CONFIGURATION || status == ERROR_GEN_FAILURE) {
            // No saved topology for this exact combination: use Windows best-mode logic.
            topologyFlags = SDC_USE_SUPPLIED_DISPLAY_CONFIG;
            topology.write(topologyFlags | SDC_VALIDATE);
        } else checkWindows(status, "Validate saved monitor layout");
    }
    try {
        State final;
        if (unchangedSet) final = impl->before; // Primary-only layout: one modeset, no database activation round-trip.
        else {
            impl->dirty = true; // A failing API can have partially changed the topology.
            topology.write(topologyFlags | SDC_APPLY);
            final = settled([&](const State& state) { verifyEnabled(state, wanted); });
        }
        restoreRetainedModes(final, impl->before);
        placeAndRebase(final, impl->before, preset.primaryMonitor);
        final.write(SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_VALIDATE);
        impl->dirty = true;
        final.write(SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_APPLY | SDC_SAVE_TO_DATABASE);
        const auto actual = settled([&](const State& state) { verifyEnabled(state, wanted); verifySame(state, final, true); });
        const auto& source = actual.source(actual.find(preset.primaryMonitor).path);
        if (source.position.x != 0 || source.position.y != 0) throw std::runtime_error("Windows did not accept the layout primary monitor.");
    } catch (const std::exception& e) {
        std::string message = e.what();
        try { restore(); message += "\nPrevious monitor layout restored."; }
        catch (const std::exception& r) { message += "\nRESTORATION FAILED: "; message += r.what(); }
        throw std::runtime_error(message);
    }
}
void MonitorLayoutTransaction::restore() {
    if (!impl->dirty) return;
    impl->before.write(SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_APPLY | SDC_SAVE_TO_DATABASE);
    settled([&](const State& state) { verifySame(state, impl->before, true); });
    impl->dirty = false;
}
}
