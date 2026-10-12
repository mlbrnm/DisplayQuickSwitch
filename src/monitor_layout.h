#pragma once
#include "backend.h"
namespace qs {
class MonitorLayoutTransaction {
public:
    MonitorLayoutTransaction();
    ~MonitorLayoutTransaction();
    MonitorLayoutTransaction(const MonitorLayoutTransaction&) = delete;
    MonitorLayoutTransaction& operator=(const MonitorLayoutTransaction&) = delete;
    void apply(const LayoutPreset& p);
    void apply(const Preset& p); // Vendor-independent resolution/refresh/primary via Windows CCD.
    void restore();
    bool needsRestore() const;
    void verifyApplied() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
Preset captureWindows(const Monitor& monitor);
}
