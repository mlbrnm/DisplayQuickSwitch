#pragma once
#include "model.h"
#include <filesystem>
namespace qs {
std::filesystem::path configPath();
std::vector<Preset> loadPresets(const std::filesystem::path& file);
void savePresets(const std::filesystem::path& file, const std::vector<Preset>& presets);
}
