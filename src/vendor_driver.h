#pragma once
#include "backend.h"
#include <functional>
namespace qs {
// Operation-scoped SDK handles. Snapshot every requested value before ANY writes.
class VendorDriver {
public:
    virtual ~VendorDriver() = default;
    virtual DriverCapabilities capabilities() = 0;
    virtual void capture(Preset& preset) = 0;
    virtual void stage(const Preset& preset) = 0;
    void apply();
    void restore();
    bool needsRestore() const;
protected:
    struct Edit { std::function<void()> write, undo, verify; bool dirty = false; };
    std::vector<Edit> edits;
};
std::unique_ptr<VendorDriver> openVendorDriver(const Monitor& monitor);
}
