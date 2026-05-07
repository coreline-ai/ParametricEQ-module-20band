#pragma once

#include "RTSafetyUtils.h"

// RAII guard that enables Flush-To-Zero / Denormals-Are-Zero for the lifetime
// of the scope, and restores the previous FPU control register on exit.
//
// Place at the top of any audio-thread entry point (e.g. process()).
class DenormalGuard {
public:
    RT_SAFE DenormalGuard() : prev_(eq_rt::enableFlushToZero()) {}
    RT_SAFE ~DenormalGuard() { eq_rt::restoreFlushToZero(prev_); }

    DenormalGuard(const DenormalGuard&) = delete;
    DenormalGuard& operator=(const DenormalGuard&) = delete;
    DenormalGuard(DenormalGuard&&) = delete;
    DenormalGuard& operator=(DenormalGuard&&) = delete;

private:
    uint32_t prev_;
};
