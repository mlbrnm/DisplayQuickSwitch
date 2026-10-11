#include "model.h"
#include "config.h"
#include "windows_scaling.h"
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
    const auto scale = decodeWindowsScale(-4, -2, 3);
    expect(scale.current == 150 && scale.recommended == 200 && scale.maximum == 300, "decode Windows relative DPI steps");
    expect(relativeWindowsScale(scale, 125) == -3 && relativeWindowsScale(scale, 300) == 3, "absolute to relative DPI steps");
    rejects([&] { relativeWindowsScale(scale, 350); }, "unsupported per-monitor scaling maximum");
    rejects([&] { relativeWindowsScale(scale, 110); }, "custom DPI percentage");
    rejects([] { decodeWindowsScale(INT32_MIN, 0, INT32_MAX); }, "DPI range overflow");
    rejects([] { decodeWindowsScale(1, 0, 3); }, "positive minimum DPI offset");
    expect(decodeWindowsScale(-2, -3, 3).current == 100, "stale DPI current offset below minimum clamps like SetDPI");
    expect(decodeWindowsScale(-2, 4, 3).current == 225, "stale DPI current offset above maximum clamps like SetDPI");
    const auto loweredRange = decodeWindowsScale(-4, 0, -1);
    expect(loweredRange.current == 175 && loweredRange.recommended == 200 && loweredRange.maximum == 175, "maximum may be below recommended step");
    expect(relativeWindowsScale(loweredRange, 100) == -4, "100 percent remains valid with negative maximum offset");
    expect(decodeWindowsScale(-2, INT32_MIN, 3).current == 100 && decodeWindowsScale(-2, INT32_MAX, 3).current == 225, "current offset clamping avoids overflow");
    rejects([] { decodeWindowsScale(-2, 0, -3); }, "absolute maximum below 100 percent is invalid");
    rejects([] { decodeWindowsScale(-2, 0, 10); }, "unknown maximum step is rejected");
    LayoutPreset layout{L"CRT only", L"crt", {{L"lcd", false}, {L"crt", true}}};
    validate(layout);
    auto badLayout = layout; badLayout.monitors[1].enabled = false;
    rejects([&] { validate(badLayout); }, "layout cannot disable every display or its primary");
    badLayout = layout; badLayout.primaryMonitor = L"missing";
    rejects([&] { validate(badLayout); }, "layout primary must be listed and enabled");
    badLayout = layout; badLayout.monitors.push_back({L"CRT", true});
    rejects([&] { validate(badLayout); }, "case-insensitive duplicate monitor identity rejected");
    Preset p; p.name = L"4K 日本語 = test"; p.monitor = L"\\\\?\\DISPLAY#ABC#123";
    p.width = 3840; p.height = 2160; p.refreshMilliHz = 119880;
    p.scaling = Scaling::integer; p.gsync = Gsync::windowed; p.vsync = Vsync::on; p.maxFps = 117;
    p.windowsScalePercent = 150;
    validate(p);
    auto invalid = p; invalid.height = 0; rejects([&] { validate(invalid); }, "partial resolution");
    invalid = p; invalid.scaling = static_cast<Scaling>(4); rejects([&] { validate(invalid); }, "invalid scaling");
    invalid = p; invalid.name = L"bad\nname"; rejects([&] { validate(invalid); }, "INI injection");
    invalid = p; invalid.maxFps = 1024; rejects([&] { validate(invalid); }, "FPS bounds");
    auto invalidScale = p; invalidScale.windowsScalePercent = 110; rejects([&] { validate(invalidScale); }, "Windows scale validation");
    wchar_t temp[32768]{}; GetTempPathW(32768, temp);
    auto directory = std::filesystem::path(temp) / (L"NvidiaQuickSwitch-tests-" + std::to_wstring(GetCurrentProcessId()));
    auto file = directory / L"presets.ini";
    auto layoutFile = directory / L"layouts.ini";
    try {
        expect(loadPresets(file).empty(), "missing file");
        auto unchanged = p; unchanged.name = L"Untouched"; unchanged.width = unchanged.height = unchanged.refreshMilliHz = 0;
        unchanged.scaling = Scaling::unchanged; unchanged.gsync = Gsync::unchanged; unchanged.vsync = Vsync::unchanged; unchanged.maxFps = -1; unchanged.primary = false;
        unchanged.windowsScalePercent = 0;
        savePresets(file, {p, unchanged});
        auto loaded = loadPresets(file);
        expect(loaded.size() == 2 && loaded[0].name == p.name && loaded[0].monitor == p.monitor &&
               loaded[0].refreshMilliHz == 119880 && loaded[0].scaling == Scaling::integer && loaded[0].maxFps == 117 && loaded[0].windowsScalePercent == 150, "Unicode and Windows scaling round-trip");
        expect(loaded[1].maxFps == -1 && loaded[1].gsync == Gsync::unchanged && !loaded[1].primary, "unchanged round-trip");
        // Strip the optional field to emulate a file written by the original release.
        savePresets(file, {p});
        {
            std::ifstream in(file, std::ios::binary);
            std::string bytes((std::istreambuf_iterator<char>(in)), {}); in.close();
            std::string field;
            for (wchar_t c : std::wstring(L"WindowsScalePercent=150\r\n")) { field += static_cast<char>(c); field += '\0'; }
            bytes.erase(bytes.find(field), field.size());
            std::ofstream out(file, std::ios::binary); out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        auto legacy = loadPresets(file);
        expect(legacy.size() == 1 && legacy[0].windowsScalePercent == 0 && legacy[0].refreshMilliHz == 119880, "v1 presets remain compatible and do not override Windows scaling");
        savePresets(file, {unchanged}); expect(loadPresets(file).size() == 1, "atomic replacement has no stale sections");
        rejects([&] { savePresets(file, {invalid}); }, "invalid save");
        expect(loadPresets(file).size() == 1, "invalid save preserves existing file");
        savePresets(file, {}); expect(loadPresets(file).empty(), "empty preset list");
        { std::ofstream out(file, std::ios::binary); out << "not a UTF16 file"; }
        rejects([&] { loadPresets(file); }, "reject malformed encoding");
        expect(loadLayouts(layoutFile).empty(), "missing layouts file");
        saveLayouts(layoutFile, {layout});
        rejects([&] { loadPresets(layoutFile); }, "layout and display-settings formats remain separate");
        auto loadedLayouts = loadLayouts(layoutFile);
        expect(loadedLayouts.size() == 1 && loadedLayouts[0].primaryMonitor == L"crt" &&
               !loadedLayouts[0].monitors[0].enabled && loadedLayouts[0].monitors[1].enabled, "layout round trip");
        rejects([&] { saveLayouts(layoutFile, {badLayout}); }, "invalid layout cannot overwrite saved layouts");
        expect(loadLayouts(layoutFile).size() == 1, "invalid layout save preserves existing file");
        saveLayouts(layoutFile, {}); expect(loadLayouts(layoutFile).empty(), "empty layouts file");
        std::filesystem::remove(layoutFile);
        std::filesystem::remove(file); std::filesystem::remove(directory);
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; ++failures; }
    if (!failures) std::cout << "All core tests passed. No display or driver APIs were called.\n";
    return failures ? 1 : 0;
}
