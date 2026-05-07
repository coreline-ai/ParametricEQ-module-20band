#include <jni.h>
#include <android/log.h>

#include <atomic>
#include <mutex>
#include <thread>
#include "FineTuneEQEngine.h"

#define JNI_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "AudioEngineJNI", __VA_ARGS__)

// Override at build time with -DEQ_JNI_CLASS_PATH='"com/mycompany/MyClass"'
// to bind to a different Kotlin/Java class without editing source.
#ifndef EQ_JNI_CLASS_PATH
#define EQ_JNI_CLASS_PATH "com/example/audio/AudioEngineJNI"
#endif

namespace {

std::atomic<FineTuneEQEngine*> g_engine{nullptr};
std::mutex g_lifecycleMutex;
std::atomic<int> g_processInFlight{0};

struct ProcessGuard {
    ProcessGuard()  { g_processInFlight.fetch_add(1, std::memory_order_acquire); }
    ~ProcessGuard() { g_processInFlight.fetch_sub(1, std::memory_order_release); }
    ProcessGuard(const ProcessGuard&) = delete;
    ProcessGuard& operator=(const ProcessGuard&) = delete;
};

void drainAndDelete(FineTuneEQEngine* old) {
    if (!old) return;
    while (g_processInFlight.load(std::memory_order_acquire) != 0) {
        std::this_thread::yield();
    }
    delete old;
}

} // namespace

static void nativeInit(JNIEnv*, jobject, jint sampleRate) {
    std::lock_guard<std::mutex> lk(g_lifecycleMutex);
    auto* fresh = new FineTuneEQEngine(static_cast<double>(sampleRate));
    auto* old = g_engine.exchange(fresh, std::memory_order_release);
    drainAndDelete(old);
}

static void nativeRelease(JNIEnv*, jobject) {
    std::lock_guard<std::mutex> lk(g_lifecycleMutex);
    auto* old = g_engine.exchange(nullptr, std::memory_order_release);
    drainAndDelete(old);
}

static void nativeUpdateConfig(JNIEnv* env, jobject,
                               jfloat preampDB,
                               jboolean enableLimiter,
                               jintArray filterTypes,
                               jfloatArray frequencies,
                               jfloatArray gains,
                               jfloatArray qFactors) {
    // Guard the engine pointer for the entire JNI call so a concurrent
    // release() cannot delete it between load() and updateConfig().
    ProcessGuard guard;
    auto* engine = g_engine.load(std::memory_order_acquire);
    if (!engine) return;

    if (!frequencies || !gains || !qFactors) {
        JNI_LOGE("updateConfig: null array argument");
        return;
    }

    jsize fLen = env->GetArrayLength(frequencies);
    jsize gLen = env->GetArrayLength(gains);
    jsize qLen = env->GetArrayLength(qFactors);
    jsize count = fLen;
    if (gLen < count) count = gLen;
    if (qLen < count) count = qLen;
    if (filterTypes) {
        jsize tLen = env->GetArrayLength(filterTypes);
        if (tLen < count) count = tLen;
    }
    if (count < 0) count = 0;
    constexpr jsize kMaxBands = 20;
    if (count > kMaxBands) count = kMaxBands;

    if (count == 0) {
        EqEngineConfig empty;
        empty.preampDB = preampDB;
        empty.enableSoftLimiter = (enableLimiter != 0);
        engine->updateConfig(empty);
        return;
    }

    jint* typeArray = filterTypes ? env->GetIntArrayElements(filterTypes, nullptr) : nullptr;
    jfloat* freqArray = env->GetFloatArrayElements(frequencies, nullptr);
    jfloat* gainArray = env->GetFloatArrayElements(gains, nullptr);
    jfloat* qArray    = env->GetFloatArrayElements(qFactors, nullptr);

    if (!freqArray || !gainArray || !qArray || (filterTypes && !typeArray)) {
        JNI_LOGE("updateConfig: GetArrayElements returned null");
        if (typeArray) env->ReleaseIntArrayElements(filterTypes, typeArray, JNI_ABORT);
        if (freqArray) env->ReleaseFloatArrayElements(frequencies, freqArray, JNI_ABORT);
        if (gainArray) env->ReleaseFloatArrayElements(gains, gainArray, JNI_ABORT);
        if (qArray)    env->ReleaseFloatArrayElements(qFactors, qArray, JNI_ABORT);
        return;
    }

    EqEngineConfig config;
    config.preampDB = preampDB;
    config.enableSoftLimiter = (enableLimiter != 0);
    config.bands.reserve(static_cast<size_t>(count));
    for (jsize i = 0; i < count; ++i) {
        EqBandConfig band;
        band.type = typeArray ? static_cast<EqFilterType>(typeArray[i]) : EqFilterType::PEAKING;
        band.frequency = freqArray[i];
        band.gainDB    = gainArray[i];
        band.qFactor   = qArray[i];
        config.bands.push_back(band);
    }

    if (typeArray) env->ReleaseIntArrayElements(filterTypes, typeArray, JNI_ABORT);
    env->ReleaseFloatArrayElements(frequencies, freqArray, JNI_ABORT);
    env->ReleaseFloatArrayElements(gains, gainArray, JNI_ABORT);
    env->ReleaseFloatArrayElements(qFactors, qArray, JNI_ABORT);

    engine->updateConfig(config);
}

static void nativeProcessDirectBuffer(JNIEnv* env, jobject,
                                      jobject buffer, jint numFrames) {
    ProcessGuard guard;

    auto* engine = g_engine.load(std::memory_order_acquire);
    if (!engine) return;
    if (numFrames <= 0) return;
    if (!buffer) return;

    void* addr = env->GetDirectBufferAddress(buffer);
    if (!addr) {
        JNI_LOGE("processDirectBuffer: GetDirectBufferAddress null");
        return;
    }

    jlong cap = env->GetDirectBufferCapacity(buffer);
    jlong required = static_cast<jlong>(numFrames) * 2 * static_cast<jlong>(sizeof(float));
    if (cap < required) {
        JNI_LOGE("processDirectBuffer: capacity %lld < required %lld",
                 static_cast<long long>(cap), static_cast<long long>(required));
        return;
    }

    float* pcm = static_cast<float*>(addr);
    engine->process(pcm, pcm, numFrames);
}

static jfloat nativeComputeAutoPreampDB(JNIEnv*, jobject) {
    // Guard the engine pointer so release() cannot delete it during the call.
    ProcessGuard guard;
    auto* engine = g_engine.load(std::memory_order_acquire);
    if (!engine) return 0.0f;
    return engine->computeAutoPreampDB();
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_audio_AudioEngineJNI_init(JNIEnv* env, jobject thiz, jint sampleRate) {
    nativeInit(env, thiz, sampleRate);
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_audio_AudioEngineJNI_release(JNIEnv* env, jobject thiz) {
    nativeRelease(env, thiz);
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_audio_AudioEngineJNI_updateConfig(JNIEnv* env, jobject thiz,
                                                   jfloat preampDB,
                                                   jboolean enableLimiter,
                                                   jintArray filterTypes,
                                                   jfloatArray frequencies,
                                                   jfloatArray gains,
                                                   jfloatArray qFactors) {
    nativeUpdateConfig(env, thiz, preampDB, enableLimiter,
                       filterTypes, frequencies, gains, qFactors);
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_audio_AudioEngineJNI_processDirectBuffer(JNIEnv* env, jobject thiz,
                                                          jobject buffer, jint numFrames) {
    nativeProcessDirectBuffer(env, thiz, buffer, numFrames);
}

extern "C" JNIEXPORT jfloat JNICALL
Java_com_example_audio_AudioEngineJNI_computeAutoPreampDB(JNIEnv* env, jobject thiz) {
    return nativeComputeAutoPreampDB(env, thiz);
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    JNIEnv* env = nullptr;
    // GetEnv failure means a fundamentally broken VM environment; fail loudly
    // so System.loadLibrary surfaces the error instead of silently degrading.
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK || !env) {
        return JNI_ERR;
    }
    // FindClass failure is acceptable: the legacy Java_com_example_audio_*
    // exported symbols still resolve via the JVM's dynamic name lookup, so
    // the library remains usable even if RegisterNatives binding can't run.
    jclass cls = env->FindClass(EQ_JNI_CLASS_PATH);
    if (!cls) {
        env->ExceptionClear();
        return JNI_VERSION_1_6;
    }
    static const JNINativeMethod kMethods[] = {
        {"init",                "(I)V",                       reinterpret_cast<void*>(&nativeInit)},
        {"release",             "()V",                        reinterpret_cast<void*>(&nativeRelease)},
        {"updateConfig",        "(FZ[I[F[F[F)V",              reinterpret_cast<void*>(&nativeUpdateConfig)},
        {"processDirectBuffer", "(Ljava/nio/ByteBuffer;I)V",  reinterpret_cast<void*>(&nativeProcessDirectBuffer)},
        {"computeAutoPreampDB", "()F",                        reinterpret_cast<void*>(&nativeComputeAutoPreampDB)},
    };
    if (env->RegisterNatives(cls, kMethods,
                             sizeof(kMethods) / sizeof(kMethods[0])) != JNI_OK) {
        env->ExceptionClear();
        JNI_LOGE("JNI_OnLoad: RegisterNatives failed; falling back to legacy C symbols");
        // Legacy C symbols are still exported, so we don't fail the load.
    }
    return JNI_VERSION_1_6;
}
