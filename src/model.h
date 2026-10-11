#pragma once
#include <string>
#include <vector>
#include <stdexcept>
#include <cstdint>

namespace qs {
enum class Scaling : int { unchanged = 0, stretch = 2, none = 3, aspect = 5, integer = 8 };
enum class Gsync : int { unchanged = -1, off = 0, fullscreen = 1, windowed = 2 };
enum class Vsync : int { unchanged = -1, application = 0, off = 1, on = 2 };
struct Preset {
    std::wstring name;
    std::wstring monitor; // Windows monitor device interface path, not DISPLAY1.
    bool primary = true;
    uint32_t width = 0, height = 0, refreshMilliHz = 0;
    Scaling scaling = Scaling::unchanged;
    Gsync gsync = Gsync::unchanged;
    Vsync vsync = Vsync::unchanged;
    int maxFps = -1; // -1 untouched, 0 disabled.
};
void validate(const Preset& p);
uint32_t parseUnsigned(const std::wstring& text, uint32_t max);
uint32_t parseRefresh(const std::wstring& text);
std::wstring formatRefresh(uint32_t milliHz);
std::wstring trim(const std::wstring& text);
struct Position { int32_t x, y; };
std::vector<Position> rebasePositions(const std::vector<Position>& positions, size_t primary);
}
