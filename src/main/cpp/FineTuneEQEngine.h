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

// Fixed-width 20-band wrapper input used by UI/JNI layers that expose
// slider arrays instead of an owned std::vector. gainMin/gainMax and
// qMin/qMax are clamp-policy bounds only; they do not change the EQ
// algorithm or coefficient formulas. Defaults represent a flat 20-band
// preset: 0 dB gains, PEAKING bands, sane frequencies, and neutral preamp.
struct Eq20BandInput {
    static constexpr int NUM_BANDS = 20;

    float gainMin = -12.0f;
    float gainMax =  12.0f;
    float gains[NUM_BANDS];

    float qMin = 0.05f;
    float qMax = 10.0f;
    float qFactors[NUM_BANDS];

    float frequencies[NUM_BANDS];
    EqFilterType filterTypes[NUM_BANDS];

    float preampDB = 0.0f;
    bool enableSoftLimiter = true;

    Eq20BandInput();
};

// Convert the fixed-width wrapper to the engine's existing vector config.
// Per-band gains and qFactors are clamped to the wrapper's min/max policy
// before coefficient generation; frequencies remain subject to the existing
// sanitize path in updateConfig().
EqEngineConfig makeEqEngineConfig(const Eq20BandInput& input);

class FineTuneEQEngine {
public:
    FineTuneEQEngine(double sampleRate);
    ~FineTuneEQEngine();

    // Update the engine configuration (thread-safe for single producer/consumer).
    // NOT_RT_SAFE — call from configuration thread; allocates and computes coeffs.
    void updateConfig(const EqEngineConfig& config);
    void updateConfig(const Eq20BandInput& input);

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
