#pragma once
#include "model.h"
#include <array>
namespace qs {
inline constexpr std::array<uint32_t, 12> WindowsScaleSteps = {100, 125, 150, 175, 200, 225, 250, 300, 350, 400, 450, 500};
struct WindowsScaleInfo { uint32_t current, recommended, maximum; };
WindowsScaleInfo decodeWindowsScale(int32_t minimumRelative, int32_t currentRelative, int32_t maximumRelative);
int32_t relativeWindowsScale(const WindowsScaleInfo& info, uint32_t percent);
WindowsScaleInfo windowsScale(const std::wstring& monitorKey); // Read-only; may reject custom/global scaling.
std::wstring windowsScaleDiagnostics(const std::wstring& monitorKey); // Read-only raw packet/range details, including failures.
void setWindowsScale(const std::wstring& monitorKey, uint32_t percent); // Resolve source afresh after topology changes.
}
