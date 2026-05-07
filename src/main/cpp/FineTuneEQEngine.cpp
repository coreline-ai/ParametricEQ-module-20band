#include "FineTuneEQEngine.h"
#include "DenormalGuard.h"
#include <cmath>
#include <algorithm>
#include <atomic>
#include <complex>
#include <cstring>
#include <mutex>

namespace {

// Returns true if all four inputs are finite and within usable RBJ ranges.
// Out-of-range inputs cause the caller to substitute a flat (unity) coefficient
// set so a single bad slider value cannot poison the entire cascade with NaN.
inline bool sanitizeBandInputs(float& f, float& g, float& Q, double fs) {
    if (!std::isfinite(f) || !std::isfinite(g) || !std::isfinite(Q)) return false;
    if (!(fs > 0.0)) return false;
    const double nyquist = fs * 0.5;
    if (f <= 0.0f || static_cast<double>(f) >= nyquist) return false;
    if (Q < 0.05f) Q = 0.05f;   // policy minimum; below this RBJ alpha blows up
    return true;
}

} // namespace

// --- Helper: Biquad Math for Peaking EQ ---
static BiquadCoeffs calculatePeakingCoeffs(float frequency, float gainDB, float qFactor, double sampleRate) {
    BiquadCoeffs coeffs;  // default = unity passthrough
    if (!sanitizeBandInputs(frequency, gainDB, qFactor, sampleRate)) return coeffs;

    double A = std::pow(10.0, gainDB / 40.0);
    double w0 = 2.0 * M_PI * frequency / sampleRate;
    double alpha = std::sin(w0) / (2.0 * qFactor);

    double b0 = 1.0 + alpha * A;
    double b1 = -2.0 * std::cos(w0);
    double b2 = 1.0 - alpha * A;
    double a0 = 1.0 + alpha / A;
    double a1 = -2.0 * std::cos(w0);
    double a2 = 1.0 - alpha / A;

    coeffs.b0 = static_cast<float>(b0 / a0);
    coeffs.b1 = static_cast<float>(b1 / a0);
    coeffs.b2 = static_cast<float>(b2 / a0);
    coeffs.a1 = static_cast<float>(a1 / a0);
    coeffs.a2 = static_cast<float>(a2 / a0);
    return coeffs;
}

static BiquadCoeffs calculateLowShelfCoeffs(float frequency, float gainDB, float qFactor, double sampleRate) {
    BiquadCoeffs coeffs;
    if (!sanitizeBandInputs(frequency, gainDB, qFactor, sampleRate)) return coeffs;

    double A = std::pow(10.0, gainDB / 40.0);
    double w0 = 2.0 * M_PI * frequency / sampleRate;
    double alpha = std::sin(w0) / (2.0 * qFactor);
    double sqrtA = std::sqrt(A);

    double b0 = A * ((A + 1.0) - (A - 1.0) * std::cos(w0) + 2.0 * sqrtA * alpha);
    double b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * std::cos(w0));
    double b2 = A * ((A + 1.0) - (A - 1.0) * std::cos(w0) - 2.0 * sqrtA * alpha);
    double a0 = (A + 1.0) + (A - 1.0) * std::cos(w0) + 2.0 * sqrtA * alpha;
    double a1 = -2.0 * ((A - 1.0) + (A + 1.0) * std::cos(w0));
    double a2 = (A + 1.0) + (A - 1.0) * std::cos(w0) - 2.0 * sqrtA * alpha;

    coeffs.b0 = static_cast<float>(b0 / a0);
    coeffs.b1 = static_cast<float>(b1 / a0);
    coeffs.b2 = static_cast<float>(b2 / a0);
    coeffs.a1 = static_cast<float>(a1 / a0);
    coeffs.a2 = static_cast<float>(a2 / a0);
    return coeffs;
}

static BiquadCoeffs calculateHighShelfCoeffs(float frequency, float gainDB, float qFactor, double sampleRate) {
    BiquadCoeffs coeffs;
    if (!sanitizeBandInputs(frequency, gainDB, qFactor, sampleRate)) return coeffs;

    double A = std::pow(10.0, gainDB / 40.0);
    double w0 = 2.0 * M_PI * frequency / sampleRate;
    double alpha = std::sin(w0) / (2.0 * qFactor);
    double sqrtA = std::sqrt(A);

    double b0 = A * ((A + 1.0) + (A - 1.0) * std::cos(w0) + 2.0 * sqrtA * alpha);
    double b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * std::cos(w0));
    double b2 = A * ((A + 1.0) + (A - 1.0) * std::cos(w0) - 2.0 * sqrtA * alpha);
    double a0 = (A + 1.0) - (A - 1.0) * std::cos(w0) + 2.0 * sqrtA * alpha;
    double a1 = 2.0 * ((A - 1.0) - (A + 1.0) * std::cos(w0));
    double a2 = (A + 1.0) - (A - 1.0) * std::cos(w0) - 2.0 * sqrtA * alpha;

    coeffs.b0 = static_cast<float>(b0 / a0);
    coeffs.b1 = static_cast<float>(b1 / a0);
    coeffs.b2 = static_cast<float>(b2 / a0);
    coeffs.a1 = static_cast<float>(a1 / a0);
    coeffs.a2 = static_cast<float>(a2 / a0);
    return coeffs;
}

// Stereo-linked soft limiter applied per stereo frame. Linking preserves the
// stereo image because both channels receive the same gain reduction.
RT_SAFE static inline void softLimitStereoFrame(float& l, float& r) {
    constexpr float threshold = 0.95f;
    constexpr float headroom  = 0.05f;
    const float absL = std::fabs(l);
    const float absR = std::fabs(r);
    const float peak = absL > absR ? absL : absR;
    if (peak > threshold) {
        const float overshoot = peak - threshold;
        const float compressed = threshold + headroom * (overshoot / (overshoot + headroom));
        const float gain = compressed / peak;
        l *= gain;
        r *= gain;
    }
}

// --- Impl Class ---
class FineTuneEQEngine::Impl {
public:
    double sampleRate;
    std::atomic<float> preampLinearGain{1.0f};
    std::atomic<bool> enableLimiter{true};

    static constexpr int NUM_BANDS = 20;
    BiquadFilter filters[NUM_BANDS];

    bool firstConfig_ = true;

    // Snapshot of the most-recently-applied target coefficients for every band.
    // Read by computeAutoPreampDB() so it never races with the audio thread's
    // active-coefficient state and always reflects the just-published config.
    // Only config-side threads (UI/JNI) touch this; mutex contention there is
    // fine and never reaches the audio callback.
    BiquadCoeffs targetSnapshot[NUM_BANDS];
    mutable std::mutex snapshotMutex;

    Impl(double sr) : sampleRate(sr) {}

    int rampSamples() const {
        int rs = static_cast<int>(sampleRate / 750.0);
        if (rs < 64) rs = 64;
        if (rs > 512) rs = 512;
        return rs;
    }
};

// --- FineTuneEQEngine Methods ---

FineTuneEQEngine::FineTuneEQEngine(double sampleRate) {
    // Reject obviously invalid sample rates so the rest of the engine can
    // assume fs > 0, finite, and within typical audio device range.
    if (!std::isfinite(sampleRate) || sampleRate < 8000.0 || sampleRate > 384000.0) {
        sampleRate = 48000.0;
    }
    pImpl = new Impl(sampleRate);
}

FineTuneEQEngine::~FineTuneEQEngine() {
    delete pImpl;
}

void FineTuneEQEngine::updateConfig(const EqEngineConfig& config) {
    // Sanitize preamp: NaN/Inf collapse to 0 dB so a single bad UI value
    // cannot make every output sample NaN.
    const float preampDB = std::isfinite(config.preampDB) ? config.preampDB : 0.0f;
    const float linearGain = std::pow(10.0f, preampDB / 20.0f);
    pImpl->preampLinearGain.store(linearGain, std::memory_order_relaxed);

    pImpl->enableLimiter.store(config.enableSoftLimiter, std::memory_order_relaxed);

    const bool first = pImpl->firstConfig_;
    const int rampSamples = pImpl->rampSamples();
    const int numBands = std::min(static_cast<int>(config.bands.size()), Impl::NUM_BANDS);

    // Compute target coefficients for ALL 20 bands. Bands beyond the user's
    // list become flat (unity passthrough) so reducing or emptying the band
    // list is a real bypass, not a stale leftover from an earlier config.
    BiquadCoeffs targets[Impl::NUM_BANDS];
    for (int i = 0; i < Impl::NUM_BANDS; ++i) {
        if (i >= numBands) {
            targets[i] = BiquadCoeffs{};  // unity
            continue;
        }
        const auto& band = config.bands[i];
        switch (band.type) {
            case EqFilterType::LOW_SHELF:
                targets[i] = calculateLowShelfCoeffs(band.frequency, band.gainDB, band.qFactor, pImpl->sampleRate);
                break;
            case EqFilterType::HIGH_SHELF:
                targets[i] = calculateHighShelfCoeffs(band.frequency, band.gainDB, band.qFactor, pImpl->sampleRate);
                break;
            case EqFilterType::PEAKING:
            default:
                targets[i] = calculatePeakingCoeffs(band.frequency, band.gainDB, band.qFactor, pImpl->sampleRate);
                break;
        }
    }

    // Publish snapshot for computeAutoPreampDB() before scheduling so a
    // concurrent caller always sees the freshest target.
    {
        std::lock_guard<std::mutex> lk(pImpl->snapshotMutex);
        std::memcpy(pImpl->targetSnapshot, targets, sizeof(targets));
    }

    for (int i = 0; i < Impl::NUM_BANDS; ++i) {
        if (first) {
            pImpl->filters[i].updateCoefficients(targets[i]);
        } else {
            pImpl->filters[i].scheduleCoefficients(targets[i], rampSamples);
        }
    }

    pImpl->firstConfig_ = false;
}

float FineTuneEQEngine::computeAutoPreampDB() const {
    constexpr int kGrid = 256;
    const double fs = pImpl->sampleRate;
    const double fLo = 20.0;
    const double fHi = (fs * 0.5) * 0.95;
    if (!(fHi > fLo)) return 0.0f;

    // Read from the config-side snapshot, NOT from each filter's active state.
    // Active state is owned by the audio thread (mid-ramp values, plain floats)
    // and reading it here would be a data race per the C++ memory model.
    BiquadCoeffs cs[Impl::NUM_BANDS];
    {
        std::lock_guard<std::mutex> lk(pImpl->snapshotMutex);
        std::memcpy(cs, pImpl->targetSnapshot, sizeof(cs));
    }

    const double logLo = std::log(fLo);
    const double logHi = std::log(fHi);
    double maxMag = 0.0;

    for (int k = 0; k < kGrid; ++k) {
        const double t = static_cast<double>(k) / static_cast<double>(kGrid - 1);
        const double f = std::exp(logLo + (logHi - logLo) * t);
        const double w = 2.0 * M_PI * f / fs;
        const std::complex<double> ejw1 = std::polar(1.0, -w);
        const std::complex<double> ejw2 = std::polar(1.0, -2.0 * w);

        double mag = 1.0;
        for (int b = 0; b < Impl::NUM_BANDS; ++b) {
            const auto& c = cs[b];
            const std::complex<double> num =
                static_cast<double>(c.b0) +
                static_cast<double>(c.b1) * ejw1 +
                static_cast<double>(c.b2) * ejw2;
            const std::complex<double> den =
                1.0 +
                static_cast<double>(c.a1) * ejw1 +
                static_cast<double>(c.a2) * ejw2;
            const double dabs = std::abs(den);
            if (dabs > 1e-30) {
                mag *= std::abs(num) / dabs;
            }
        }
        if (mag > maxMag) maxMag = mag;
    }

    if (maxMag > 1.0) {
        return static_cast<float>(-20.0 * std::log10(maxMag));
    }
    return 0.0f;
}

void FineTuneEQEngine::process(const float* inputPCM, float* outputPCM, int numFrames) {
    DenormalGuard guard;

    const float preamp = pImpl->preampLinearGain.load(std::memory_order_relaxed);
    const bool limit = pImpl->enableLimiter.load(std::memory_order_relaxed);

    BiquadFilter* filters = pImpl->filters;

    // Drain the lock-free publication slot once per block so any pending
    // scheduled coefficients are picked up before we enter the fused loop.
    // process(buffer, 0) is a no-op except for the internal pullPublishedTarget.
    for (int b = 0; b < Impl::NUM_BANDS; ++b) {
        filters[b].process(outputPCM, 0);
    }

    for (int i = 0; i < numFrames; ++i) {
        float l = inputPCM[2 * i]     * preamp;
        float r = inputPCM[2 * i + 1] * preamp;

        for (int b = 0; b < Impl::NUM_BANDS; ++b) {
            l = filters[b].processSampleL(l);
            r = filters[b].processSampleR(r);
            filters[b].tickRamp();
        }

        if (limit) {
            softLimitStereoFrame(l, r);
        }

        outputPCM[2 * i]     = l;
        outputPCM[2 * i + 1] = r;
    }
}
