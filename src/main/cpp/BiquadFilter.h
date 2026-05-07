#pragma once

#include <atomic>
#include <cstdint>
#include "RTSafetyUtils.h"

struct BiquadCoeffs {
    float b0 = 1.0f;
    float b1 = 0.0f;
    float b2 = 0.0f;
    float a1 = 0.0f;
    float a2 = 0.0f;
};

// Stateful Biquad Filter (Transposed Direct Form II) for interleaved stereo audio.
//
// Concurrency model:
//   - One audio thread calls process()/processSampleL()/processSampleR() (RT-safe).
//   - Any non-RT thread (e.g. UI/JNI) may call updateCoefficients() or
//     scheduleCoefficients() to publish new coefficients.
//   - Coefficient publication is lock-free (double-buffered, seqlock-style)
//     so the audio thread never observes a torn struct.
//
// Click suppression:
//   - scheduleCoefficients(target, rampSamples) starts a sample-accurate
//     linear ramp from the currently-active coefficients toward `target`.
//   - process() ticks the ramp internally.
class BiquadFilter {
public:
    BiquadFilter();
    ~BiquadFilter() = default;

    BiquadFilter(const BiquadFilter&) = delete;
    BiquadFilter& operator=(const BiquadFilter&) = delete;

    // Publish new coefficients with an instantaneous swap (no ramp).
    // NOT_RT_SAFE — call from configuration thread only.
    NOT_RT_SAFE void updateCoefficients(const BiquadCoeffs& newCoeffs);

    // Publish new coefficients with a sample-accurate linear ramp from the
    // currently active coefficients. rampSamples must be > 0.
    // NOT_RT_SAFE — call from configuration thread only.
    NOT_RT_SAFE void scheduleCoefficients(const BiquadCoeffs& target, int rampSamples);

    // Reset delay lines to zero. Safe to call alongside updates from the
    // configuration thread but must not race with process() on the audio thread.
    void reset();

    // Process interleaved stereo PCM in-place. RT-safe.
    // numFrames is the number of stereo pairs (numSamples = numFrames * 2).
    RT_SAFE void process(float* buffer, int numFrames);

    // Single-sample fast path used by the engine's cascade-fusion loop.
    // The caller is responsible for advancing the ramp once per stereo frame
    // by calling tickRamp() between channel pairs.
    RT_SAFE inline float processSampleL(float in) {
        const float out = in * activeB0_ + z1_L_;
        z1_L_ = in * activeB1_ - out * activeA1_ + z2_L_;
        z2_L_ = in * activeB2_ - out * activeA2_;
        return out;
    }

    RT_SAFE inline float processSampleR(float in) {
        const float out = in * activeB0_ + z1_R_;
        z1_R_ = in * activeB1_ - out * activeA1_ + z2_R_;
        z2_R_ = in * activeB2_ - out * activeA2_;
        return out;
    }

    // Advance the coefficient ramp by one stereo frame.
    // Safe to call every frame even when no ramp is active (cheap no-op).
    RT_SAFE void tickRamp();

    // For diagnostics/testing only.
    BiquadCoeffs activeCoeffs() const;

private:
    // Pull the latest published coefficients (if any) from the lock-free slot.
    // Returns true if a new target was picked up.
    RT_SAFE bool pullPublishedTarget();

    // --- Active (live) coefficients used by process loops ---
    float activeB0_ = 1.0f;
    float activeB1_ = 0.0f;
    float activeB2_ = 0.0f;
    float activeA1_ = 0.0f;
    float activeA2_ = 0.0f;

    // --- Ramp state ---
    float targetB0_ = 1.0f;
    float targetB1_ = 0.0f;
    float targetB2_ = 0.0f;
    float targetA1_ = 0.0f;
    float targetA2_ = 0.0f;
    float stepB0_ = 0.0f;
    float stepB1_ = 0.0f;
    float stepB2_ = 0.0f;
    float stepA1_ = 0.0f;
    float stepA2_ = 0.0f;
    int rampRemaining_ = 0;

    // --- Delay lines (per channel) ---
    float z1_L_ = 0.0f;
    float z2_L_ = 0.0f;
    float z1_R_ = 0.0f;
    float z2_R_ = 0.0f;

    // --- Lock-free publication slot (seqlock-style) ---
    // Writer increments seq_ to odd, writes pendingX_, increments to even.
    // Reader (audio thread) snapshots seq_ before/after copy and retries on tear.
    // Payload coefficients are individual atomics with relaxed ordering so the
    // tooling (TSan) sees concurrent access as well-defined; the release/acquire
    // ordering on publishSeq_ provides the actual cross-thread happens-before.
    std::atomic<float> pendingB0_{1.0f};
    std::atomic<float> pendingB1_{0.0f};
    std::atomic<float> pendingB2_{0.0f};
    std::atomic<float> pendingA1_{0.0f};
    std::atomic<float> pendingA2_{0.0f};
    std::atomic<int> pendingRampSamples_{0};
    std::atomic<uint32_t> publishSeq_{0};
    std::atomic<uint32_t> consumedSeq_{0};
};
