#include "vendor_driver.h"
#include <algorithm>
namespace qs {
void VendorDriver::apply() {
    for (auto& e : edits) { e.dirty = true; e.write(); }
    // A later driver edit can affect an earlier one; verify the complete final policy.
    for (auto& e : edits) e.verify();
}
bool VendorDriver::needsRestore() const {
    return std::any_of(edits.begin(), edits.end(), [](auto& e) { return e.dirty; });
}
void VendorDriver::restore() {
    std::string errors;
    for (auto i = edits.rbegin(); i != edits.rend(); ++i) if (i->dirty) {
        try { i->undo(); i->dirty = false; }
        catch (const std::exception& e) { if (!errors.empty()) errors += "\n"; errors += e.what(); }
    }
    if (!errors.empty()) throw std::runtime_error(errors);
}
}
