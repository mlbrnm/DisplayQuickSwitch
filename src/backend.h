#pragma once
#include "model.h"
#include <memory>
namespace qs {
struct Monitor {
    std::wstring key, name, gdiName;
    bool primary = false;
};
std::vector<Monitor> monitors(); // Read-only Windows enumeration, including stable interface paths.
Preset capture(const Monitor& monitor); // Read-only NVAPI + DRS.
std::wstring diagnostics(); // Read-only; never applies a mode.
class Transaction {
public:
    Transaction();
    ~Transaction();
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    void apply(const Preset& preset); // Snapshot, validate, apply, read back. Throws with rollback status.
    void restore(); // Best effort on BOTH display and driver state; throws if either fails.
    bool needsRestore() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
