#include "windows_scaling.h"
#include <algorithm>
namespace qs {
WindowsScaleInfo decodeWindowsScale(int32_t minimumRelative, int32_t currentRelative, int32_t maximumRelative) {
    // The first step is 100%; the recommended step is the negative of the minimum offset.
    const int64_t recommended = -static_cast<int64_t>(minimumRelative);
    const int64_t current = recommended + currentRelative;
    const int64_t maximum = recommended + maximumRelative;
    const auto count = static_cast<int64_t>(WindowsScaleSteps.size());
    if (minimumRelative > 0 || maximumRelative < 0 || recommended >= count ||
        maximum < recommended || maximum >= count || current < 0 || current > maximum)
        throw std::runtime_error("Windows reported an unsupported DPI scaling range. Leave Windows scaling unchanged.");
    return {WindowsScaleSteps[static_cast<size_t>(current)], WindowsScaleSteps[static_cast<size_t>(recommended)], WindowsScaleSteps[static_cast<size_t>(maximum)]};
}
int32_t relativeWindowsScale(const WindowsScaleInfo& info, uint32_t percent) {
    const auto step = std::find(WindowsScaleSteps.begin(), WindowsScaleSteps.end(), percent);
    const auto recommended = std::find(WindowsScaleSteps.begin(), WindowsScaleSteps.end(), info.recommended);
    if (step == WindowsScaleSteps.end() || recommended == WindowsScaleSteps.end() || percent > info.maximum)
        throw std::runtime_error("Requested Windows scaling is not supported at this monitor's current resolution.");
    return static_cast<int32_t>(step - recommended);
}
}
