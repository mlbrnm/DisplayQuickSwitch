#include "model.h"
#include "config.h"
#include <windows.h>
#include <fstream>
#include <iostream>
#include <functional>

int failures = 0;
void expect(bool value, const char* label) { if (!value) { std::cerr << "FAIL: " << label << '\n'; ++failures; } }
void rejects(const std::function<void()>& fn, const char* label) {
    try { fn(); expect(false, label); } catch (const std::exception&) {}
}
int main() {
    using namespace qs;
    expect(parseRefresh(L"59.94") == 59940, "fractional refresh");
    expect(parseRefresh(L"119.880") == 119880, "three-digit refresh");
    expect(parseRefresh(L"") == 0, "blank refresh unchanged");
    expect(formatRefresh(59940) == L"59.94", "refresh format");
    expect(formatRefresh(120000) == L"120", "whole refresh format");
    for (auto s : {L"0", L"-1", L"60.0001", L"60.", L"60Hz", L".5", L"1000.001", L"1e2", L"429496729600000"})
        rejects([&] { parseRefresh(s); }, "invalid refresh");
    rejects([] { parseUnsigned(L"999999999999999999999999", 1023); }, "numeric overflow");
    auto positions = rebasePositions({{-3840, 0}, {0, 0}, {1920, -200}}, 0);
    expect(positions[0].x == 0 && positions[1].x == 3840 && positions[2].x == 5760 && positions[2].y == -200, "primary translation preserves layout");
    rejects([] { rebasePositions({{INT32_MIN, 0}, {INT32_MAX, 0}}, 0); }, "layout overflow");
    rejects([] { rebasePositions({}, 0); }, "missing primary");
    Preset p; p.name = L"4K 日本語 = test"; p.monitor = L"\\\\?\\DISPLAY#ABC#123";
    p.width = 3840; p.height = 2160; p.refreshMilliHz = 119880;
    p.scaling = Scaling::integer; p.gsync = Gsync::windowed; p.vsync = Vsync::on; p.maxFps = 117;
    validate(p);
    auto invalid = p; invalid.height = 0; rejects([&] { validate(invalid); }, "partial resolution");
    invalid = p; invalid.scaling = static_cast<Scaling>(4); rejects([&] { validate(invalid); }, "invalid scaling");
    invalid = p; invalid.name = L"bad\nname"; rejects([&] { validate(invalid); }, "INI injection");
    invalid = p; invalid.maxFps = 1024; rejects([&] { validate(invalid); }, "FPS bounds");
    wchar_t temp[32768]{}; GetTempPathW(32768, temp);
    auto directory = std::filesystem::path(temp) / (L"NvidiaQuickSwitch-tests-" + std::to_wstring(GetCurrentProcessId()));
    auto file = directory / L"presets.ini";
    try {
        expect(loadPresets(file).empty(), "missing file");
        auto unchanged = p; unchanged.name = L"Untouched"; unchanged.width = unchanged.height = unchanged.refreshMilliHz = 0;
        unchanged.scaling = Scaling::unchanged; unchanged.gsync = Gsync::unchanged; unchanged.vsync = Vsync::unchanged; unchanged.maxFps = -1; unchanged.primary = false;
        savePresets(file, {p, unchanged});
        auto loaded = loadPresets(file);
        expect(loaded.size() == 2 && loaded[0].name == p.name && loaded[0].monitor == p.monitor &&
               loaded[0].refreshMilliHz == 119880 && loaded[0].scaling == Scaling::integer && loaded[0].maxFps == 117, "Unicode round-trip");
        expect(loaded[1].maxFps == -1 && loaded[1].gsync == Gsync::unchanged && !loaded[1].primary, "unchanged round-trip");
        savePresets(file, {unchanged}); expect(loadPresets(file).size() == 1, "atomic replacement has no stale sections");
        rejects([&] { savePresets(file, {invalid}); }, "invalid save");
        expect(loadPresets(file).size() == 1, "invalid save preserves existing file");
        savePresets(file, {}); expect(loadPresets(file).empty(), "empty preset list");
        { std::ofstream out(file, std::ios::binary); out << "not a UTF16 file"; }
        rejects([&] { loadPresets(file); }, "reject malformed encoding");
        std::filesystem::remove(file); std::filesystem::remove(directory);
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; ++failures; }
    if (!failures) std::cout << "All core tests passed. No display or driver APIs were called.\n";
    return failures ? 1 : 0;
}
