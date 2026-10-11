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
    void restore();
    bool needsRestore() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
