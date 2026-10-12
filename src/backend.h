#pragma once
#include "model.h"
#include <memory>
namespace qs {
enum class Vendor { unknown, nvidia, amd, intel };
struct Monitor {
    std::wstring key, name, gdiName;
    bool primary = false;
    bool enabled = true;
    bool connected = true;
    Vendor vendor = Vendor::unknown;
    uint32_t adapterLow = 0, targetId = 0;
    int32_t adapterHigh = 0;
};
struct DriverCapabilities {
    bool scaling = false, integerScaling = false, vrr = false, vsync = false, fps = false;
    int minFps = 1, maxFps = 1023, fpsStep = 1;
    std::wstring notice;
};
DriverCapabilities driverCapabilities(const Monitor& monitor); // Read-only, per target GPU/driver.
bool usesNvidiaTopology(const std::vector<Monitor>& displays);
std::vector<Monitor> monitors(); // Read-only Windows enumeration, including stable interface paths.
std::vector<Monitor> connectedMonitors(); // Includes physically connected but Windows-disabled displays.
Preset capture(const Monitor& monitor); // Read-only Windows mode + available vendor driver settings.
std::wstring diagnostics(); // Read-only; never applies a mode.
class Transaction {
public:
    Transaction();
    ~Transaction();
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    void apply(const Preset& preset); // Snapshot, validate, apply, read back. Throws with rollback status.
    void apply(const LayoutPreset& preset); // Only enabled/disabled state and primary; no DRS/DPI edits.
    void restore(); // Best effort on BOTH display and driver state; throws if either fails.
    bool needsRestore() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
