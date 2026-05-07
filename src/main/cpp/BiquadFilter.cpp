#include "BiquadFilter.h"

BiquadFilter::BiquadFilter() = default;

void BiquadFilter::updateCoefficients(const BiquadCoeffs& c) {
    // Seqlock writer: bump to odd, write payload, bump to even.
    // Release on the even-store publishes all the relaxed payload writes
    // to any reader that performs an acquire-load and observes that even seq.
    const uint32_t seq = publishSeq_.load(std::memory_order_relaxed);
    publishSeq_.store(seq + 1, std::memory_order_release);
    pendingB0_.store(c.b0, std::memory_order_relaxed);
    pendingB1_.store(c.b1, std::memory_order_relaxed);
    pendingB2_.store(c.b2, std::memory_order_relaxed);
    pendingA1_.store(c.a1, std::memory_order_relaxed);
    pendingA2_.store(c.a2, std::memory_order_relaxed);
    pendingRampSamples_.store(0, std::memory_order_relaxed);
    publishSeq_.store(seq + 2, std::memory_order_release);
}

void BiquadFilter::scheduleCoefficients(const BiquadCoeffs& target, int rampSamples) {
    if (rampSamples <= 0) {
        updateCoefficients(target);
        return;
    }
    const uint32_t seq = publishSeq_.load(std::memory_order_relaxed);
    publishSeq_.store(seq + 1, std::memory_order_release);
    pendingB0_.store(target.b0, std::memory_order_relaxed);
    pendingB1_.store(target.b1, std::memory_order_relaxed);
    pendingB2_.store(target.b2, std::memory_order_relaxed);
    pendingA1_.store(target.a1, std::memory_order_relaxed);
    pendingA2_.store(target.a2, std::memory_order_relaxed);
    pendingRampSamples_.store(rampSamples, std::memory_order_relaxed);
    publishSeq_.store(seq + 2, std::memory_order_release);
}

void BiquadFilter::reset() {
    z1_L_ = 0.0f; z2_L_ = 0.0f;
    z1_R_ = 0.0f; z2_R_ = 0.0f;
}

bool BiquadFilter::pullPublishedTarget() {
    // Seqlock reader: snapshot seq, copy payload, re-read seq, retry on tear.
    // Bounded by a small spin count so the audio thread never blocks
    // indefinitely on a writer that was preempted mid-publish.
    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint32_t s1 = publishSeq_.load(std::memory_order_acquire);
        if ((s1 & 1u) != 0u) {
            continue;
        }
        if (s1 == consumedSeq_.load(std::memory_order_relaxed)) {
            return false;
        }
        BiquadCoeffs local;
        local.b0 = pendingB0_.load(std::memory_order_relaxed);
        local.b1 = pendingB1_.load(std::memory_order_relaxed);
        local.b2 = pendingB2_.load(std::memory_order_relaxed);
        local.a1 = pendingA1_.load(std::memory_order_relaxed);
        local.a2 = pendingA2_.load(std::memory_order_relaxed);
        const int ramp = pendingRampSamples_.load(std::memory_order_relaxed);
        const uint32_t s2 = publishSeq_.load(std::memory_order_acquire);
        if (s1 != s2) {
            continue;
        }
        consumedSeq_.store(s1, std::memory_order_relaxed);

        targetB0_ = local.b0; targetB1_ = local.b1; targetB2_ = local.b2;
        targetA1_ = local.a1; targetA2_ = local.a2;

        if (ramp <= 0) {
            activeB0_ = targetB0_; activeB1_ = targetB1_; activeB2_ = targetB2_;
            activeA1_ = targetA1_; activeA2_ = targetA2_;
            stepB0_ = stepB1_ = stepB2_ = stepA1_ = stepA2_ = 0.0f;
            rampRemaining_ = 0;
        } else {
            const float inv = 1.0f / static_cast<float>(ramp);
            stepB0_ = (targetB0_ - activeB0_) * inv;
            stepB1_ = (targetB1_ - activeB1_) * inv;
            stepB2_ = (targetB2_ - activeB2_) * inv;
            stepA1_ = (targetA1_ - activeA1_) * inv;
            stepA2_ = (targetA2_ - activeA2_) * inv;
            rampRemaining_ = ramp;
        }
        return true;
    }
    return false;
}

void BiquadFilter::tickRamp() {
    if (rampRemaining_ > 0) {
        activeB0_ += stepB0_;
        activeB1_ += stepB1_;
        activeB2_ += stepB2_;
        activeA1_ += stepA1_;
        activeA2_ += stepA2_;
        if (--rampRemaining_ == 0) {
            // Snap to exact target on the final tick to eliminate accumulated
            // floating-point drift from the linear interpolation.
            activeB0_ = targetB0_;
            activeB1_ = targetB1_;
            activeB2_ = targetB2_;
            activeA1_ = targetA1_;
            activeA2_ = targetA2_;
        }
    }
}

void BiquadFilter::process(float* buffer, int numFrames) {
    pullPublishedTarget();
    for (int i = 0; i < numFrames; ++i) {
        float l = buffer[2 * i];
        float r = buffer[2 * i + 1];
        l = processSampleL(l);
        r = processSampleR(r);
        buffer[2 * i] = l;
        buffer[2 * i + 1] = r;
        tickRamp();
    }
}

BiquadCoeffs BiquadFilter::activeCoeffs() const {
    BiquadCoeffs c;
    c.b0 = activeB0_; c.b1 = activeB1_; c.b2 = activeB2_;
    c.a1 = activeA1_; c.a2 = activeA2_;
    return c;
}
