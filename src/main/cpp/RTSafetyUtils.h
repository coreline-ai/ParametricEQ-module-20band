#pragma once

// RT-safe utilities for the AndroidEQModule audio engine.
//
// Marker macros and small helpers that document real-time safety contracts
// between the audio callback path and the configuration/lifecycle path.
//
// Marker is informational only; tooling can grep for it.
#define RT_SAFE        // function must not allocate, lock, syscall, or block
#define NOT_RT_SAFE    // function may allocate / lock — never call from audio callback

#include <cstdint>

#if defined(__ARM_NEON) || defined(__aarch64__)
    #define EQ_HAS_NEON 1
    #include <arm_neon.h>
#else
    #define EQ_HAS_NEON 0
#endif

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_IX86_FP)
    #define EQ_HAS_SSE 1
    #include <xmmintrin.h>
    #include <pmmintrin.h>
#else
    #define EQ_HAS_SSE 0
#endif

namespace eq_rt {

// Enable Flush-To-Zero / Denormals-Are-Zero on the calling thread.
// Returns the previous register value so it can be restored later.
inline uint32_t enableFlushToZero() {
#if defined(__aarch64__)
    uint64_t fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    uint64_t newFpcr = fpcr | (1ULL << 24); // FZ
    __asm__ volatile("msr fpcr, %0" : : "r"(newFpcr));
    return static_cast<uint32_t>(fpcr);
#elif defined(__ARM_NEON)
    uint32_t fpscr;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
    uint32_t newFpscr = fpscr | (1u << 24); // FZ bit
    __asm__ volatile("vmsr fpscr, %0" : : "r"(newFpscr));
    return fpscr;
#elif EQ_HAS_SSE
    uint32_t prev = _mm_getcsr();
    _mm_setcsr(prev | 0x8040u); // FTZ | DAZ
    return prev;
#else
    return 0;
#endif
}

inline void restoreFlushToZero(uint32_t prev) {
#if defined(__aarch64__)
    uint64_t v = prev;
    __asm__ volatile("msr fpcr, %0" : : "r"(v));
#elif defined(__ARM_NEON)
    __asm__ volatile("vmsr fpscr, %0" : : "r"(prev));
#elif EQ_HAS_SSE
    _mm_setcsr(prev);
#else
    (void)prev;
#endif
}

} // namespace eq_rt
