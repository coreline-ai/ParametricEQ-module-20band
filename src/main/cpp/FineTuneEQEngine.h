#pragma once

#include <vector>
#include "BiquadFilter.h"

enum class EqFilterType {
    PEAKING = 0,
    LOW_SHELF = 1,
    HIGH_SHELF = 2
};

// Configuration for a single EQ band.
// All fields default to a neutral (no-op) PEAKING setting so partially-
// initialized instances do not feed garbage into the RBJ formulas.
struct EqBandConfig {
    EqFilterType type = EqFilterType::PEAKING;
    float frequency   = 1000.0f;  // Hz
    float gainDB      = 0.0f;     // dB; positive = boost, negative = cut
    float qFactor     = 0.707f;   // Butterworth (sqrt(2)/2)
};

// Global Configuration for the EQ Engine.
// Bands list may contain 0..20 entries; missing bands are treated as flat
// passthrough so removing a band is a real bypass, not a stale leftover.
struct EqEngineConfig {
    float preampDB           = 0.0f;   // dB; negative recommended for boost-heavy presets
    bool  enableSoftLimiter  = true;   // safer default
    std::vector<EqBandConfig> bands;
};

class FineTuneEQEngine {
public:
    FineTuneEQEngine(double sampleRate);
    ~FineTuneEQEngine();

    // Update the engine configuration (thread-safe for single producer/consumer).
    // NOT_RT_SAFE — call from configuration thread; allocates and computes coeffs.
    void updateConfig(const EqEngineConfig& config);

    // Core DSP processing for interleaved stereo Float32 buffers.
    // Handles Preamp -> 20-Band Biquad Cascade -> Soft Limiter.
    // inputPCM and outputPCM can point to the same memory for in-place processing.
    // RT_SAFE — must not allocate, lock, or block.
    void process(const float* inputPCM, float* outputPCM, int numFrames);

    // Recommended preamp (in dB, typically negative) such that the cumulative
    // peak magnitude of the configured 20-band cascade does not exceed 0 dBFS.
    // Sampling-grid based estimator (~256 log-spaced points). NOT_RT_SAFE.
    // Returns 0.0f when the cascade is purely attenuating.
    float computeAutoPreampDB() const;

private:
    class Impl;
    Impl* pImpl;
};
