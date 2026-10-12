#include "config.h"
#include <windows.h>
#include <fstream>
#include <map>

namespace qs {
std::filesystem::path configPath() {
    wchar_t value[32768]{};
    auto n = GetEnvironmentVariableW(L"LOCALAPPDATA", value, 32768);
    if (!n || n >= 32768) throw std::runtime_error("LOCALAPPDATA is unavailable.");
    auto directory = std::filesystem::path(value) / L"DisplayQuickSwitch";
    // Copy existing data once, without deleting originals or overwriting new-version files.
    auto legacy = std::filesystem::path(value) / L"NvidiaQuickSwitch";
    for (auto name : {L"presets.ini", L"layouts.ini"}) {
        if (!std::filesystem::exists(directory / name) && std::filesystem::exists(legacy / name)) {
            std::filesystem::create_directories(directory);
            std::filesystem::copy_file(legacy / name, directory / name, std::filesystem::copy_options::skip_existing);
        }
    }
    return directory / L"presets.ini";
}
std::filesystem::path layoutConfigPath() { return configPath().parent_path() / L"layouts.ini"; }
namespace {
std::vector<std::map<std::wstring, std::wstring>> readSections(const std::filesystem::path& file, const std::wstring& section) {
    if (!std::filesystem::exists(file)) return {};
    if (std::filesystem::file_size(file) > 1024 * 1024) throw std::runtime_error("Preset file is too large.");
    std::ifstream input(file, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(input)), {});
    if (bytes.size() < 2 || static_cast<unsigned char>(bytes[0]) != 0xff || static_cast<unsigned char>(bytes[1]) != 0xfe || bytes.size() % 2)
        throw std::runtime_error("Preset file must be UTF-16 LE (use the built-in editor).");
    std::wstring text;
    for (size_t i = 2; i < bytes.size(); i += 2)
        text += static_cast<wchar_t>(static_cast<unsigned char>(bytes[i]) | (static_cast<unsigned char>(bytes[i + 1]) << 8));
    std::vector<std::map<std::wstring, std::wstring>> sections;
    size_t start = 0;
    while (start < text.size()) {
        auto end = text.find(L'\n', start);
        auto line = trim(text.substr(start, end == std::wstring::npos ? end : end - start));
        start = end == std::wstring::npos ? text.size() : end + 1;
        if (line.empty() || line.front() == L';') continue;
        if (line.front() == L'[' && line.back() == L']') {
            if (line != section) throw std::runtime_error("Unknown section in preset file.");
            sections.emplace_back();
            if (sections.size() > 100) throw std::runtime_error("At most 100 presets are supported.");
        } else {
            auto eq = line.find(L'=');
            if (sections.empty() || eq == std::wstring::npos) throw std::runtime_error("Invalid preset file.");
            if (!sections.back().emplace(trim(line.substr(0, eq)), trim(line.substr(eq + 1))).second)
                throw std::runtime_error("Duplicate field in preset file.");
        }
    }
    return sections;
}
void writeConfig(const std::filesystem::path& file, const std::wstring& text) {
    std::filesystem::create_directories(file.parent_path());
    auto temp = file; temp += L".tmp";
    HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot write presets.");
    DWORD written = 0;
    const wchar_t bom = 0xfeff;
    const auto size = static_cast<DWORD>(text.size() * sizeof(wchar_t));
    bool ok = WriteFile(h, &bom, 2, &written, nullptr) && written == 2;
    ok = ok && WriteFile(h, text.data(), size, &written, nullptr) && written == size;
    ok = ok && FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || !MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str());
        throw std::runtime_error("Cannot save presets atomically. The original file was left intact.");
    }
}
}
std::vector<Preset> loadPresets(const std::filesystem::path& file) {
    auto sections = readSections(file, L"[Preset]");
    std::vector<Preset> out;
    for (auto& s : sections) {
        const std::vector<std::wstring> keys = {L"Name", L"Monitor", L"Primary", L"Width", L"Height", L"RefreshMilliHz", L"Scaling", L"Gsync", L"Vsync", L"MaxFps"};
        // The new field is optional when reading v1 files; existing presets stay unchanged.
        const auto expected = keys.size() + (s.contains(L"WindowsScalePercent") ? 1 : 0);
        if (s.size() != expected) throw std::runtime_error("Missing or unknown preset fields.");
        for (const auto& key : keys) if (!s.contains(key)) throw std::runtime_error("Missing preset field.");
        auto number = [&](const wchar_t* key, uint32_t max) { return parseUnsigned(s.at(key), max); };
        auto optional = [&](const wchar_t* key, uint32_t max) { return s.at(key) == L"-1" ? -1 : static_cast<int>(number(key, max)); };
        Preset p;
        p.name = s.at(L"Name"); p.monitor = s.at(L"Monitor");
        p.primary = number(L"Primary", 1) != 0;
        p.width = number(L"Width", 32768); p.height = number(L"Height", 32768);
        p.refreshMilliHz = number(L"RefreshMilliHz", 1000000);
        p.scaling = static_cast<Scaling>(number(L"Scaling", 8));
        if (s.contains(L"WindowsScalePercent")) p.windowsScalePercent = number(L"WindowsScalePercent", 500);
        p.gsync = static_cast<Gsync>(optional(L"Gsync", 2));
        p.vsync = static_cast<Vsync>(optional(L"Vsync", 2));
        p.maxFps = optional(L"MaxFps", 1023);
        validate(p); out.push_back(std::move(p));
    }
    return out;
}
void savePresets(const std::filesystem::path& file, const std::vector<Preset>& presets) {
    if (presets.size() > 100) throw std::runtime_error("At most 100 presets are supported.");
    std::wstring text = L"; Display Quick Switch v2. Driver setting scope varies by vendor. WindowsScalePercent=0 means unchanged.\r\n";
    for (const auto& p : presets) {
        validate(p);
        text += L"\r\n[Preset]\r\nName=" + trim(p.name) + L"\r\nMonitor=" + p.monitor;
        auto field = [&](const wchar_t* k, int64_t v) { text += L"\r\n" + std::wstring(k) + L"=" + std::to_wstring(v); };
        field(L"Primary", p.primary); field(L"Width", p.width); field(L"Height", p.height);
        field(L"RefreshMilliHz", p.refreshMilliHz); field(L"Scaling", static_cast<int>(p.scaling));
        field(L"WindowsScalePercent", p.windowsScalePercent);
        field(L"Gsync", static_cast<int>(p.gsync)); field(L"Vsync", static_cast<int>(p.vsync)); field(L"MaxFps", p.maxFps);
        text += L"\r\n";
    }
    writeConfig(file, text);
}
std::vector<LayoutPreset> loadLayouts(const std::filesystem::path& file) {
    auto sections = readSections(file, L"[Layout]");
    std::vector<LayoutPreset> result;
    for (auto& s : sections) {
        if (!s.contains(L"Name") || !s.contains(L"Primary") || !s.contains(L"MonitorCount")) throw std::runtime_error("Missing layout fields.");
        auto count = parseUnsigned(s.at(L"MonitorCount"), 64);
        if (s.size() != 3 + count * 2) throw std::runtime_error("Missing or unknown layout fields.");
        LayoutPreset p; p.name = s.at(L"Name"); p.primaryMonitor = s.at(L"Primary");
        for (uint32_t i = 0; i < count; ++i) {
            auto suffix = std::to_wstring(i), monitor = L"Monitor" + suffix, enabled = L"Enabled" + suffix;
            if (!s.contains(monitor) || !s.contains(enabled)) throw std::runtime_error("Missing monitor entry in layout.");
            p.monitors.push_back({s.at(monitor), parseUnsigned(s.at(enabled), 1) != 0});
        }
        validate(p); result.push_back(std::move(p));
    }
    return result;
}
void saveLayouts(const std::filesystem::path& file, const std::vector<LayoutPreset>& presets) {
    if (presets.size() > 100) throw std::runtime_error("At most 100 monitor layout presets are supported.");
    std::wstring text = L"; Monitor layouts: enabled state and primary only. No display/driver settings.\r\n";
    for (const auto& p : presets) {
        validate(p);
        text += L"\r\n[Layout]\r\nName=" + trim(p.name) + L"\r\nPrimary=" + p.primaryMonitor + L"\r\nMonitorCount=" + std::to_wstring(p.monitors.size());
        for (size_t i = 0; i < p.monitors.size(); ++i) {
            text += L"\r\nMonitor" + std::to_wstring(i) + L"=" + p.monitors[i].monitor;
            text += L"\r\nEnabled" + std::to_wstring(i) + L"=" + (p.monitors[i].enabled ? L"1" : L"0");
        }
        text += L"\r\n";
    }
    writeConfig(file, text);
}
}
