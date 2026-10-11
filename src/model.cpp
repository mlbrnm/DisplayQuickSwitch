#include "model.h"
#include "windows_scaling.h"
#include <algorithm>
#include <limits>

namespace qs {
std::wstring trim(const std::wstring& s) {
    const auto a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return {};
    return s.substr(a, s.find_last_not_of(L" \t\r\n") - a + 1);
}
uint32_t parseUnsigned(const std::wstring& text, uint32_t max) {
    auto s = trim(text);
    if (s.empty()) throw std::runtime_error("A number is required.");
    uint64_t n = 0;
    for (wchar_t c : s) {
        if (c < L'0' || c > L'9') throw std::runtime_error("Use a non-negative whole number.");
        n = n * 10 + (c - L'0');
        if (n > max) throw std::runtime_error("Number is outside the supported range.");
    }
    return static_cast<uint32_t>(n);
}
uint32_t parseRefresh(const std::wstring& text) {
    auto s = trim(text);
    if (s.empty()) return 0;
    const auto dot = s.find(L'.');
    auto whole = parseUnsigned(s.substr(0, dot), 1000);
    uint32_t fraction = 0;
    if (dot != std::wstring::npos) {
        auto f = s.substr(dot + 1);
        if (f.empty() || f.size() > 3) throw std::runtime_error("Refresh rate supports up to three decimal places.");
        fraction = parseUnsigned(f, 999);
        for (size_t i = f.size(); i < 3; ++i) fraction *= 10;
    }
    const auto result = whole * 1000 + fraction;
    if (result < 1000 || result > 1000000) throw std::runtime_error("Refresh rate must be between 1 and 1000 Hz.");
    return result;
}
std::wstring formatRefresh(uint32_t n) {
    if (!n) return {};
    auto s = std::to_wstring(n / 1000);
    if (n % 1000) {
        auto f = std::to_wstring(1000 + n % 1000).substr(1);
        while (f.back() == L'0') f.pop_back();
        s += L"." + f;
    }
    return s;
}
void validate(const Preset& p) {
    if (p.windowsScalePercent && std::find(WindowsScaleSteps.begin(), WindowsScaleSteps.end(), p.windowsScalePercent) == WindowsScaleSteps.end())
        throw std::runtime_error("Choose a standard Windows scaling percentage, or leave it unchanged.");
    if (trim(p.name).empty() || p.name.size() > 100 || p.name.find_first_of(L"\r\n\t") != std::wstring::npos)
        throw std::runtime_error("Preset name must be 1-100 characters without line breaks or tabs.");
    if (p.monitor.empty() || p.monitor.size() > 1024 || p.monitor.find_first_of(L"\r\n") != std::wstring::npos)
        throw std::runtime_error("Select a target monitor.");
    if ((p.width == 0) != (p.height == 0) || p.width > 32768 || p.height > 32768)
        throw std::runtime_error("Specify both resolution dimensions, or leave both blank.");
    if (p.refreshMilliHz && (p.refreshMilliHz < 1000 || p.refreshMilliHz > 1000000))
        throw std::runtime_error("Refresh rate must be between 1 and 1000 Hz.");
    if (p.scaling != Scaling::unchanged && p.scaling != Scaling::stretch && p.scaling != Scaling::none &&
        p.scaling != Scaling::aspect && p.scaling != Scaling::integer)
        throw std::runtime_error("Invalid GPU scaling mode.");
    if (static_cast<int>(p.gsync) < -1 || static_cast<int>(p.gsync) > 2 ||
        static_cast<int>(p.vsync) < -1 || static_cast<int>(p.vsync) > 2 || p.maxFps < -1 || p.maxFps > 1023)
        throw std::runtime_error("Invalid global driver setting.");
}
std::vector<Position> rebasePositions(const std::vector<Position>& in, size_t primary) {
    if (primary >= in.size()) throw std::runtime_error("Primary display was not found.");
    auto out = in;
    for (auto& p : out) {
        const auto x = static_cast<int64_t>(p.x) - in[primary].x;
        const auto y = static_cast<int64_t>(p.y) - in[primary].y;
        if (x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX)
            throw std::runtime_error("Display position overflow.");
        p = {static_cast<int32_t>(x), static_cast<int32_t>(y)};
    }
    return out;
}
}
