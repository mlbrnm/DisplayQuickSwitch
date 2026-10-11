#include "windows_scaling.h"
#include <algorithm>
namespace qs {
WindowsScaleInfo decodeWindowsScale(int32_t minimumRelative, int32_t currentRelative, int32_t maximumRelative) {
    // The first step is 100%; the recommended step is the negative of the minimum offset.
    const int64_t recommended = -static_cast<int64_t>(minimumRelative);
    const int64_t maximum = recommended + maximumRelative;
    const auto count = static_cast<int64_t>(WindowsScaleSteps.size());
    // SetDPI treats currentRelative as a value to clamp, not as evidence that the
    // whole range is invalid. Windows can retain an old offset across a modeset.
    // Likewise, maxRelative can be negative: the recommended step need not be
    // available at the current resolution. Only the ABSOLUTE bounds must be valid.
    if (minimumRelative > 0 || recommended >= count || maximum < 0 || maximum >= count)
        throw std::runtime_error("Windows reported an unrecognized DPI scaling range (min=" + std::to_string(minimumRelative) +
            ", current=" + std::to_string(currentRelative) + ", max=" + std::to_string(maximumRelative) + "). Leave Windows scaling unchanged.");
    const auto clampedCurrent = std::clamp(static_cast<int64_t>(currentRelative), static_cast<int64_t>(minimumRelative), static_cast<int64_t>(maximumRelative));
    const auto current = recommended + clampedCurrent;
    return {WindowsScaleSteps[static_cast<size_t>(current)], WindowsScaleSteps[static_cast<size_t>(recommended)], WindowsScaleSteps[static_cast<size_t>(maximum)]};
}
int32_t relativeWindowsScale(const WindowsScaleInfo& info, uint32_t percent) {
    const auto step = std::find(WindowsScaleSteps.begin(), WindowsScaleSteps.end(), percent);
    const auto recommended = std::find(WindowsScaleSteps.begin(), WindowsScaleSteps.end(), info.recommended);
    if (step == WindowsScaleSteps.end() || recommended == WindowsScaleSteps.end() || percent > info.maximum)
        throw std::runtime_error("Requested Windows scaling " + std::to_string(percent) + "% is outside the recognized range of 100%-" +
            std::to_string(info.maximum) + "% (recommended " + std::to_string(info.recommended) + "%).");
    return static_cast<int32_t>(step - recommended);
}
}
