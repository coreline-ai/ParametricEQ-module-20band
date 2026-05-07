#include "FineTuneEQEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr double kSampleRate = 48000.0;
constexpr float kPi = 3.14159265358979323846f;

struct TestFailure {
    std::string name;
    std::string detail;
};

bool isFiniteBuffer(const std::vector<float>& buffer) {
    return std::all_of(buffer.begin(), buffer.end(), [](float v) {
        return std::isfinite(v);
    });
}

float maxAbs(const std::vector<float>& buffer) {
    float peak = 0.0f;
    for (float v : buffer) {
        peak = std::max(peak, std::fabs(v));
    }
    return peak;
}

std::vector<float> makeStereoSine(int frames,
                                  float frequency,
                                  float amplitude,
                                  double sampleRate = kSampleRate) {
    std::vector<float> pcm(static_cast<size_t>(frames) * 2U);
    for (int i = 0; i < frames; ++i) {
        const float s = amplitude * std::sin(2.0f * kPi * frequency * static_cast<float>(i) /
                                             static_cast<float>(sampleRate));
        pcm[static_cast<size_t>(2 * i)] = s;
        pcm[static_cast<size_t>(2 * i + 1)] = -0.75f * s;
    }
    return pcm;
}

float maxDiff(const std::vector<float>& a, const std::vector<float>& b) {
    const size_t n = std::min(a.size(), b.size());
    float diff = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        diff = std::max(diff, std::fabs(a[i] - b[i]));
    }
    return diff;
}

EqEngineConfig makeTwentyBandConfig() {
    EqEngineConfig config;
    config.preampDB = 6.0f;
    config.enableSoftLimiter = true;

    const float freqs[20] = {
        31.25f, 44.2f, 62.5f, 88.4f, 125.0f,
        176.8f, 250.0f, 353.6f, 500.0f, 707.1f,
        1000.0f, 1414.2f, 2000.0f, 2828.4f, 4000.0f,
        5656.9f, 8000.0f, 11313.7f, 14000.0f, 18000.0f
    };

    config.bands.reserve(20);
    for (int i = 0; i < 20; ++i) {
        EqBandConfig band;
        band.type = EqFilterType::PEAKING;
        band.frequency = freqs[i];
        band.gainDB = (i % 4 == 0) ? 5.0f : ((i % 4 == 1) ? -3.0f : ((i % 4 == 2) ? 2.5f : -1.5f));
        band.qFactor = 0.75f + 0.03f * static_cast<float>(i % 5);
        config.bands.push_back(band);
    }

    return config;
}

bool pipelineSmoke(TestFailure& failure) {
    FineTuneEQEngine engine(kSampleRate);
    engine.updateConfig(makeTwentyBandConfig());

    const int frames = 4096;
    const auto input = makeStereoSine(frames, 997.0f, 0.9f);
    std::vector<float> output(input.size(), 0.0f);
    engine.process(input.data(), output.data(), frames);

    const float peak = maxAbs(output);
    if (!isFiniteBuffer(output)) {
        failure = {"pipeline smoke", "output contains NaN or Inf"};
        return false;
    }
    if (peak > 1.01f) {
        failure = {"pipeline smoke", "limiter peak exceeded expected headroom: " + std::to_string(peak)};
        return false;
    }
    return true;
}

bool emptyConfigRemovesStaleFilters(TestFailure& failure) {
    FineTuneEQEngine engine(kSampleRate);

    EqEngineConfig boosted;
    boosted.preampDB = 0.0f;
    boosted.enableSoftLimiter = false;
    boosted.bands.push_back({EqFilterType::PEAKING, 1000.0f, 12.0f, 1.0f});
    engine.updateConfig(boosted);

    auto warmup = makeStereoSine(2048, 1000.0f, 0.1f);
    engine.process(warmup.data(), warmup.data(), 2048);

    EqEngineConfig empty;
    empty.preampDB = 0.0f;
    empty.enableSoftLimiter = false;
    engine.updateConfig(empty);

    std::vector<float> silence(4096 * 2U, 0.0f);
    engine.process(silence.data(), silence.data(), 4096);

    const int frames = 2048;
    const auto input = makeStereoSine(frames, 1000.0f, 0.2f);
    std::vector<float> output(input.size(), 0.0f);
    engine.process(input.data(), output.data(), frames);

    float maxDiff = 0.0f;
    for (size_t i = 0; i < input.size(); ++i) {
        maxDiff = std::max(maxDiff, std::fabs(input[i] - output[i]));
    }
    if (!isFiniteBuffer(output)) {
        failure = {"empty config stale filter 제거", "output contains NaN or Inf"};
        return false;
    }
    if (maxDiff > 1.0e-3f) {
        failure = {"empty config stale filter 제거", "empty config did not return close to passthrough; max diff=" +
                                                   std::to_string(maxDiff)};
        return false;
    }
    return true;
}

bool invalidParameterSafety(TestFailure& failure) {
    FineTuneEQEngine engine(std::numeric_limits<double>::quiet_NaN());

    EqEngineConfig config;
    config.preampDB = std::numeric_limits<float>::quiet_NaN();
    config.enableSoftLimiter = true;
    config.bands.push_back({EqFilterType::PEAKING, std::numeric_limits<float>::quiet_NaN(), 6.0f, 1.0f});
    config.bands.push_back({EqFilterType::LOW_SHELF, -10.0f, std::numeric_limits<float>::infinity(), 0.5f});
    config.bands.push_back({EqFilterType::HIGH_SHELF, 24000.0f, 3.0f, -1.0f});
    engine.updateConfig(config);

    const int frames = 1024;
    const auto input = makeStereoSine(frames, 440.0f, 0.5f);
    std::vector<float> output(input.size(), 0.0f);
    engine.process(input.data(), output.data(), frames);

    if (!isFiniteBuffer(output)) {
        failure = {"invalid parameter safety", "sanitized-invalid config still produced NaN or Inf"};
        return false;
    }
    if (maxAbs(output) > 1.01f) {
        failure = {"invalid parameter safety", "unexpected unsafe output peak=" + std::to_string(maxAbs(output))};
        return false;
    }
    return true;
}

bool nonFinitePcmInputDoesNotPoisonState(TestFailure& failure) {
    const float badValues[] = {
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
    };

    for (float badValue : badValues) {
        FineTuneEQEngine engine(kSampleRate);
        engine.updateConfig(makeTwentyBandConfig());

        const int frames = 1024;
        const auto normal = makeStereoSine(frames, 997.0f, 0.25f);

        std::vector<float> warmupOut(normal.size(), 0.0f);
        engine.process(normal.data(), warmupOut.data(), frames);
        if (!isFiniteBuffer(warmupOut)) {
            failure = {"non-finite PCM input recovery", "normal warmup output contains NaN or Inf"};
            return false;
        }

        auto badInput = normal;
        badInput[0] = badValue;
        badInput[1] = -badValue;
        std::vector<float> badOut(badInput.size(), 0.0f);
        engine.process(badInput.data(), badOut.data(), frames);
        if (!isFiniteBuffer(badOut)) {
            failure = {"non-finite PCM input recovery", "bad PCM input leaked NaN or Inf to output"};
            return false;
        }
        if (maxAbs(badOut) > 1.01f) {
            failure = {"non-finite PCM input recovery", "bad PCM input exceeded limiter tolerance"};
            return false;
        }

        std::vector<float> recoveryOut(normal.size(), 0.0f);
        engine.process(normal.data(), recoveryOut.data(), frames);
        if (!isFiniteBuffer(recoveryOut)) {
            failure = {"non-finite PCM input recovery", "filter state stayed poisoned after bad PCM input"};
            return false;
        }
        if (maxAbs(recoveryOut) > 1.01f) {
            failure = {"non-finite PCM input recovery", "recovery output exceeded limiter tolerance"};
            return false;
        }
    }

    return true;
}

bool nonFiniteCascadeOutputResetsState(TestFailure& failure) {
    FineTuneEQEngine engine(kSampleRate);

    EqEngineConfig config;
    config.preampDB = 0.0f;
    config.enableSoftLimiter = true;
    config.bands.push_back({EqFilterType::PEAKING, 1000.0f, 24.0f, 1.0f});
    engine.updateConfig(config);

    constexpr int frames = 64;
    std::vector<float> overload(static_cast<size_t>(frames) * 2U, 0.0f);
    overload[0] = std::numeric_limits<float>::max();
    overload[1] = -std::numeric_limits<float>::max();

    std::vector<float> overloadOut(overload.size(), 0.0f);
    engine.process(overload.data(), overloadOut.data(), frames);
    if (!isFiniteBuffer(overloadOut)) {
        failure = {"non-finite cascade output reset", "overflowing finite PCM leaked NaN or Inf to output"};
        return false;
    }

    const int recoveryFrames = 1024;
    const auto recovery = makeStereoSine(recoveryFrames, 997.0f, 0.2f);
    std::vector<float> recoveryOut(recovery.size(), 0.0f);
    engine.process(recovery.data(), recoveryOut.data(), recoveryFrames);
    if (!isFiniteBuffer(recoveryOut)) {
        failure = {"non-finite cascade output reset", "filter state stayed poisoned after overflow recovery reset"};
        return false;
    }
    if (maxAbs(recoveryOut) > 1.01f) {
        failure = {"non-finite cascade output reset", "recovery output exceeded limiter tolerance"};
        return false;
    }

    return true;
}

bool inPlaceOutOfPlaceEquivalence(TestFailure& failure) {
    FineTuneEQEngine inPlaceEngine(kSampleRate);
    FineTuneEQEngine outOfPlaceEngine(kSampleRate);
    const EqEngineConfig config = makeTwentyBandConfig();
    inPlaceEngine.updateConfig(config);
    outOfPlaceEngine.updateConfig(config);

    const int frames = 4096;
    const auto input = makeStereoSine(frames, 777.0f, 0.35f);
    auto inPlace = input;
    std::vector<float> outOfPlace(input.size(), 0.0f);

    inPlaceEngine.process(inPlace.data(), inPlace.data(), frames);
    outOfPlaceEngine.process(input.data(), outOfPlace.data(), frames);

    float maxDiff = 0.0f;
    for (size_t i = 0; i < inPlace.size(); ++i) {
        maxDiff = std::max(maxDiff, std::fabs(inPlace[i] - outOfPlace[i]));
    }
    if (!isFiniteBuffer(inPlace) || !isFiniteBuffer(outOfPlace)) {
        failure = {"in-place/out-of-place equivalence", "output contains NaN or Inf"};
        return false;
    }
    if (maxDiff > 1.0e-6f) {
        failure = {"in-place/out-of-place equivalence", "max diff=" + std::to_string(maxDiff)};
        return false;
    }
    return true;
}

bool autoPreampImmediateForPlus12dB(TestFailure& failure) {
    FineTuneEQEngine engine(kSampleRate);

    EqEngineConfig config;
    config.preampDB = 0.0f;
    config.enableSoftLimiter = true;
    config.bands.push_back({EqFilterType::PEAKING, 1000.0f, 12.0f, 1.0f});
    engine.updateConfig(config);

    const float autoPreampDB = engine.computeAutoPreampDB();
    if (!std::isfinite(autoPreampDB)) {
        failure = {"auto preamp immediate +12dB 근처", "auto preamp is not finite"};
        return false;
    }
    if (autoPreampDB > -10.5f || autoPreampDB < -13.5f) {
        failure = {"auto preamp immediate +12dB 근처", "expected roughly -12 dB, got " +
                                                        std::to_string(autoPreampDB)};
        return false;
    }
    return true;
}

bool wrapperClampAndEquivalence(TestFailure& failure) {
    Eq20BandInput input;
    input.gainMin = -5.0f;
    input.gainMax = 5.0f;
    input.qMin = 0.3f;
    input.qMax = 5.0f;

    for (int i = 0; i < Eq20BandInput::NUM_BANDS; ++i) {
        input.filterTypes[i] = EqFilterType::PEAKING;
        input.frequencies[i] = 100.0f * static_cast<float>(i + 1);
        input.gains[i] = 10.0f;      // must clamp to +5 dB
        input.qFactors[i] = 0.01f;   // must clamp to 0.3
    }

    const EqEngineConfig converted = makeEqEngineConfig(input);
    if (converted.bands.size() != Eq20BandInput::NUM_BANDS) {
        failure = {"wrapper clamp/equivalence", "converted band count is not 20"};
        return false;
    }
    for (const auto& band : converted.bands) {
        if (std::fabs(band.gainDB - 5.0f) > 1.0e-6f) {
            failure = {"wrapper clamp/equivalence", "gain was not clamped to gainMax"};
            return false;
        }
        if (std::fabs(band.qFactor - 0.3f) > 1.0e-6f) {
            failure = {"wrapper clamp/equivalence", "Q was not clamped to qMin"};
            return false;
        }
    }

    FineTuneEQEngine wrapperEngine(kSampleRate);
    FineTuneEQEngine configEngine(kSampleRate);
    wrapperEngine.updateConfig(input);
    configEngine.updateConfig(converted);

    const int frames = 2048;
    const auto source = makeStereoSine(frames, 1000.0f, 0.2f);
    auto wrapperOut = source;
    std::vector<float> configOut(source.size(), 0.0f);
    wrapperEngine.process(wrapperOut.data(), wrapperOut.data(), frames);
    configEngine.process(source.data(), configOut.data(), frames);

    float maxDiff = 0.0f;
    for (size_t i = 0; i < source.size(); ++i) {
        maxDiff = std::max(maxDiff, std::fabs(wrapperOut[i] - configOut[i]));
    }
    if (maxDiff > 1.0e-6f) {
        failure = {"wrapper clamp/equivalence", "wrapper/config output mismatch; max diff=" +
                                                  std::to_string(maxDiff)};
        return false;
    }
    return true;
}

bool sampleRateSupportMatrix(TestFailure& failure) {
    const double sampleRates[] = {
        44100.0, 48000.0, 88200.0, 96000.0,
        176400.0, 192000.0, 352800.0, 384000.0
    };

    for (double sampleRate : sampleRates) {
        FineTuneEQEngine engine(sampleRate);
        engine.updateConfig(makeTwentyBandConfig());

        const int frames = 1024;
        const auto input = makeStereoSine(frames, 997.0f, 0.35f, sampleRate);
        std::vector<float> output(input.size(), 0.0f);
        engine.process(input.data(), output.data(), frames);

        const float autoPreampDB = engine.computeAutoPreampDB();
        if (!isFiniteBuffer(output)) {
            failure = {"sample-rate support matrix", "non-finite output at " + std::to_string(sampleRate) + " Hz"};
            return false;
        }
        if (!std::isfinite(autoPreampDB)) {
            failure = {"sample-rate support matrix", "non-finite auto preamp at " + std::to_string(sampleRate) + " Hz"};
            return false;
        }
        if (maxAbs(output) > 1.01f) {
            failure = {"sample-rate support matrix", "limiter peak exceeded at " + std::to_string(sampleRate) +
                                                     " Hz: " + std::to_string(maxAbs(output))};
            return false;
        }

        Eq20BandInput wrapper;
        wrapper.gains[10] = 3.0f;
        wrapper.qFactors[10] = 1.2f;
        engine.updateConfig(wrapper);
        std::fill(output.begin(), output.end(), 0.0f);
        engine.process(input.data(), output.data(), frames);
        if (!isFiniteBuffer(output)) {
            failure = {"sample-rate support matrix", "wrapper preset non-finite output at " +
                                                     std::to_string(sampleRate) + " Hz"};
            return false;
        }
    }

    return true;
}

bool unsupportedSampleRateFallsBackTo48000(TestFailure& failure) {
    const double unsupportedRates[] = {
        44099.0,
        384001.0,
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(),
        0.0,
        -1.0
    };

    EqEngineConfig config = makeTwentyBandConfig();
    config.preampDB = 0.0f;
    config.enableSoftLimiter = false;

    const int frames = 2048;
    const auto input = makeStereoSine(frames, 1234.0f, 0.05f, 48000.0);

    for (double unsupportedRate : unsupportedRates) {
        FineTuneEQEngine unsupportedEngine(unsupportedRate);
        FineTuneEQEngine referenceEngine(48000.0);
        unsupportedEngine.updateConfig(config);
        referenceEngine.updateConfig(config);

        std::vector<float> unsupportedOut(input.size(), 0.0f);
        std::vector<float> referenceOut(input.size(), 0.0f);
        unsupportedEngine.process(input.data(), unsupportedOut.data(), frames);
        referenceEngine.process(input.data(), referenceOut.data(), frames);

        if (!isFiniteBuffer(unsupportedOut)) {
            failure = {"unsupported sample-rate fallback", "non-finite output for unsupported sampleRate"};
            return false;
        }
        const float diff = maxDiff(unsupportedOut, referenceOut);
        if (diff > 1.0e-6f) {
            failure = {"unsupported sample-rate fallback", "unsupported sampleRate did not match 48 kHz fallback; max diff=" +
                                                            std::to_string(diff)};
            return false;
        }

        const float unsupportedAuto = unsupportedEngine.computeAutoPreampDB();
        const float referenceAuto = referenceEngine.computeAutoPreampDB();
        if (!std::isfinite(unsupportedAuto) || std::fabs(unsupportedAuto - referenceAuto) > 1.0e-5f) {
            failure = {"unsupported sample-rate fallback", "auto preamp did not match 48 kHz fallback"};
            return false;
        }
    }

    return true;
}

bool sameRateNoResamplingFrameInvariant(TestFailure& failure) {
    struct Case {
        double sampleRate;
        int frames;
    };

    const Case cases[] = {
        {44100.0, 1024},
        {48000.0, 480},
        {384000.0, 256}
    };

    constexpr float kSentinel = -12345.0f;
    constexpr size_t kGuardSamples = 16;

    for (const auto& c : cases) {
        FineTuneEQEngine engine(c.sampleRate);
        EqEngineConfig config = makeTwentyBandConfig();
        engine.updateConfig(config);

        const auto input = makeStereoSine(c.frames, 440.0f, 0.2f, c.sampleRate);
        std::vector<float> output(input.size() + kGuardSamples, kSentinel);
        engine.process(input.data(), output.data(), c.frames);

        std::vector<float> written(output.begin(), output.begin() + static_cast<long>(input.size()));
        if (!isFiniteBuffer(written)) {
            failure = {"same-rate no-resampling frame invariant", "non-finite written region at " +
                                                                 std::to_string(c.sampleRate) + " Hz"};
            return false;
        }
        for (size_t i = input.size(); i < output.size(); ++i) {
            if (output[i] != kSentinel) {
                failure = {"same-rate no-resampling frame invariant", "wrote beyond numFrames at " +
                                                                     std::to_string(c.sampleRate) + " Hz"};
                return false;
            }
        }
    }

    return true;
}

bool nyquistFrequencyPolicy(TestFailure& failure) {
    {
        FineTuneEQEngine engine(44100.0);
        EqEngineConfig config;
        config.enableSoftLimiter = false;
        config.bands.push_back({EqFilterType::PEAKING, 23000.0f, 12.0f, 1.0f});
        engine.updateConfig(config);

        const int frames = 1024;
        const auto input = makeStereoSine(frames, 1000.0f, 0.2f, 44100.0);
        std::vector<float> output(input.size(), 0.0f);
        engine.process(input.data(), output.data(), frames);

        const float diff = maxDiff(input, output);
        if (!isFiniteBuffer(output)) {
            failure = {"nyquist frequency policy", "23 kHz / 44.1 kHz invalid band produced non-finite output"};
            return false;
        }
        if (diff > 1.0e-6f) {
            failure = {"nyquist frequency policy", "frequency >= Nyquist should become flat; max diff=" +
                                                   std::to_string(diff)};
            return false;
        }
    }

    {
        FineTuneEQEngine engine(44100.0);
        EqEngineConfig config;
        config.enableSoftLimiter = false;
        config.bands.push_back({EqFilterType::PEAKING, 20000.0f, 6.0f, 1.0f});
        engine.updateConfig(config);

        const int frames = 2048;
        const auto input = makeStereoSine(frames, 20000.0f, 0.05f, 44100.0);
        std::vector<float> output(input.size(), 0.0f);
        engine.process(input.data(), output.data(), frames);

        const float diff = maxDiff(input, output);
        if (!isFiniteBuffer(output)) {
            failure = {"nyquist frequency policy", "20 kHz / 44.1 kHz valid band produced non-finite output"};
            return false;
        }
        if (diff < 1.0e-4f) {
            failure = {"nyquist frequency policy", "20 kHz / 44.1 kHz valid band looked unexpectedly flat"};
            return false;
        }
    }

    {
        FineTuneEQEngine engine(384000.0);
        EqEngineConfig config;
        config.enableSoftLimiter = false;
        config.bands.push_back({EqFilterType::PEAKING, 100000.0f, 3.0f, 1.0f});
        engine.updateConfig(config);

        const int frames = 1024;
        const auto input = makeStereoSine(frames, 100000.0f, 0.05f, 384000.0);
        std::vector<float> output(input.size(), 0.0f);
        engine.process(input.data(), output.data(), frames);
        if (!isFiniteBuffer(output)) {
            failure = {"nyquist frequency policy", "100 kHz / 384 kHz valid band produced non-finite output"};
            return false;
        }
    }

    return true;
}

} // namespace

int main() {
    std::vector<bool (*)(TestFailure&)> tests = {
        pipelineSmoke,
        emptyConfigRemovesStaleFilters,
        invalidParameterSafety,
        nonFinitePcmInputDoesNotPoisonState,
        nonFiniteCascadeOutputResetsState,
        inPlaceOutOfPlaceEquivalence,
        autoPreampImmediateForPlus12dB,
        wrapperClampAndEquivalence,
        sampleRateSupportMatrix,
        unsupportedSampleRateFallsBackTo48000,
        sameRateNoResamplingFrameInvariant,
        nyquistFrequencyPolicy,
    };

    for (auto test : tests) {
        TestFailure failure;
        if (!test(failure)) {
            std::cerr << "FAIL: " << failure.name << " - " << failure.detail << '\n';
            return EXIT_FAILURE;
        }
    }

    std::cout << "PASS: standalone regression harness" << '\n';
    return EXIT_SUCCESS;
}
