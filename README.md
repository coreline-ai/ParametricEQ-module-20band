<div align="center">

<img width="2752" height="1536" alt="스마트폰용 초강력 20밴드 EQ 모듈" src="https://github.com/user-attachments/assets/6da86bae-76e4-4f2d-bca7-68fa92db2401" />

# 🎧 Android 20-Band Parametric EQ Module

[![C++](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white)](https://en.cppreference.com/w/cpp/17)
[![Android NDK](https://img.shields.io/badge/Android%20NDK-r27-3DDC84?logo=android&logoColor=white)](https://developer.android.com/ndk)
[![CMake](https://img.shields.io/badge/CMake-3.22%2B-064F8C?logo=cmake&logoColor=white)](https://cmake.org/)
[![ABIs](https://img.shields.io/badge/ABIs-arm64--v8a%20%7C%20armv7a%20%7C%20x86__64-blue)](#-build--abi-matrix)
[![DSP](https://img.shields.io/badge/DSP-RBJ%20Cookbook-orange)](https://webaudio.github.io/Audio-EQ-Cookbook/audio-eq-cookbook.html)
[![Real-Time Safe](https://img.shields.io/badge/Real--Time-Lock--Free-green)](#%EF%B8%8F-real-time-safety-guarantees)
[![Sanitizers](https://img.shields.io/badge/TSan%20%2F%20ASan%20%2F%20UBSan-Clean-brightgreen)](#-verification--quality-gates)
[![Production Ready](https://img.shields.io/badge/Production-Ready-success)](#-production-readiness)
[![License](https://img.shields.io/badge/License-Proprietary-lightgrey)](#-license)

**Android 환경의 lock-free, real-time safe 20-band Parametric EQ — 프로덕션급 독립형 C++ DSP 코어**

RBJ Audio EQ Cookbook 표준 수식 · Stereo interleaved Float32 · JNI Direct ByteBuffer · Oboe/AAudio/AudioTrack 어떤 I/O와도 결합 가능

[Overview](#-overview) · [Scope](#-scope) · [EQ Comparison](#-eq-type-comparison) · [Quick Start](#-quick-start) · [API](#-api-reference) · [Build](#-build--abi-matrix) · [Verification](#-verification--quality-gates) · [Production](#-production-readiness) · [Developer Guide](./DEVELOPER_GUIDE.md)

</div>

---

## 🎯 Overview

이 모듈은 안드로이드(또는 일반 C++) 환경에서 오디오 스트림에 **20-band Parametric EQ**를 적용하는 독립형 C++ DSP 코어입니다. 오디오 I/O 계층(Oboe/AAudio/AudioTrack)과 DSP 연산을 완전히 분리한 블랙박스로 설계되어, 어떤 출력 파이프라인에 끼워 넣어도 동일하게 동작합니다.

**프로덕션 환경 가정** — 음악 플레이어, 헤드폰 보정, 톤 조절 같이 사용자가 EQ 슬라이더를 실시간 드래그하고 화면 전환·앱 백그라운드 토글이 빈번한 모바일 환경에서, 클릭 노이즈 / UAF 크래시 / data race 없이 동작하도록 설계되었습니다.

### 신호 흐름

```text
   ┌──────────┐   ┌───────────────────────────────┐   ┌──────────────────┐
   │  Preamp  │──▶│ 20-Band Biquad Cascade        │──▶│  Soft Limiter    │──▶ Output
   │  (gain)  │   │ (Stateful TDF-II,             │   │ (stereo-linked,  │   PCM
   └──────────┘   │  lock-free coefficient publish│   │  optional)       │   Float32
        ▲         │  + sample-accurate ramp)      │   └──────────────────┘
        │         └───────────────────────────────┘            ▲
        │                       ▲                              │
   ┌────┴───────────────────────┴──────────────────────────────┴──────┐
   │  updateConfig() — UI/JNI thread (lock-free, click-free 64~512 ramp) │
   └─────────────────────────────────────────────────────────────────────┘
```

| 단계 | 역할 |
|---|---|
| **Preamp** | 누적 캐스케이드 최대 게인을 상쇄해 헤드룸 확보. `computeAutoPreampDB()`가 256-point log-grid \|H(f)\| 평가로 권장값 자동 산출 |
| **20-Band Cascade** | 샘플 단위 `processSampleL/R` 직렬 통과 (cascade fusion). 계수는 lock-free seqlock으로 발행, 64~512 sample 선형 ramp 후 종점 snap |
| **Soft Limiter** | L/R 동일 게인으로 동시 감쇠 (stereo image 보존). asymptotic soft-knee, threshold 0.95, headroom 0.05 |

---

## 🎯 Scope

본 모듈의 범위는 **20-band Parametric EQ**로 한정됩니다.

- **포함**: `PEAKING`, `LOW_SHELF`, `HIGH_SHELF` biquad cascade, preamp, soft limiter, JNI bridge, fixed-width wrapper input
- **제외**: Linear-phase EQ, Dynamic EQ, Oboe player 구현, Android UI 구현
- **Sample-rate 계약**: `44100Hz ~ 384000Hz` same-rate DSP. `input PCM sampleRate == FineTuneEQEngine sampleRate == output PCM sampleRate`
- **Resampling 제외**: 본 모듈은 sample-rate converter가 아니며, upsampling/downsampling을 수행하지 않습니다.
- Linear-phase / Dynamic EQ는 비교 대상 또는 제외 범위로만 언급되며, 본 문서의 구현 계획에 포함되지 않습니다.

---

## 📊 EQ Type Comparison

본 모듈이 **왜 Parametric EQ만 포함하고 다른 두 종류는 제외했는지** 결정 근거.

| 구분 | Parametric EQ | Linear-phase EQ | Dynamic EQ |
|---|---|---|---|
| **기본 개념** | 특정 주파수 대역을 고정적으로 올리거나 내림 | 주파수 보정 시 위상 왜곡을 최소화 | 특정 대역의 소리 크기에 따라 EQ가 자동으로 움직임 |
| **동작 방식** | `frequency`, `gain`, `Q`로 IIR biquad 적용 (RBJ Cookbook) | FIR / FFT / convolution 기반 선형 위상 필터 | EQ + compressor 결합. threshold·attack·release 기반 |
| **Gain 변화** | 항상 고정 (정적) | 항상 고정 (정적) | 입력 신호 레벨에 따라 실시간 변화 |
| **Phase 영향** | 있음 (IIR 필연) | 거의 없음 (linear-phase) | 일반적으로 있음 |
| **Latency** | 낮음 (수 sample) | 높음 (수십~수백 ms, FIR 길이 비례) | 낮음~중간 (envelope follower) |
| **CPU 비용** | 낮음 | 높음 (FFT/convolution) | 중간~높음 |
| **Pre-ringing** | 없음 | 있음 (transient에서 가청) | 없음 |
| **Android 실시간 적합성** | 🟢 높음 | 🔴 낮음 | 🟡 중간 |
| **본 프로젝트 포함** | ✅ **포함** | ❌ 제외 | ❌ 제외 |

> 📌 **선택 근거**: Android 실시간 콜백 (10~20ms 버퍼) + 모바일 SoC CPU 예산 + 음악 앱/헤드폰 보정 용도에선 Parametric EQ가 latency·CPU·결정성 모두 가장 적합. Linear-phase / Dynamic EQ가 필요해지면 별도 모듈로 추가하되 본 모듈 시그니처는 변경하지 않는 방향이 안전.

---

## 📦 Features

### 🎛️ 지원 필터 타입

| 타입 | 용도 | 파라미터 |
|---|---|---|
| `PEAKING` (Bell) | 특정 대역 boost / cut | freq, gain, **Q** (bandwidth) |
| `LOW_SHELF` | 저역 일괄 boost / cut | freq, gain, **S** (shelf slope) |
| `HIGH_SHELF` | 고역 일괄 boost / cut | freq, gain, **S** (shelf slope) |

> ⚠️ **중요**: `EqBandConfig::qFactor` 필드는 PEAKING에선 Q (bandwidth)이고, LOW_SHELF/HIGH_SHELF에선 RBJ shelf slope S (transition steepness)로 해석됩니다. 동일 필드명이지만 type에 따라 의미가 다릅니다. 수식은 [RBJ Audio EQ Cookbook](https://webaudio.github.io/Audio-EQ-Cookbook/audio-eq-cookbook.html)을 100% 따릅니다.

### 🛡️ Real-Time Safety Guarantees

프로덕션 모바일 오디오 콜백(10~20ms 데드라인)을 가정하고 설계한 안전 계약입니다.

| 보장 | 메커니즘 |
|---|---|
| **Lock-free coefficient publication** | Seqlock-style 슬롯 (5×`std::atomic<float>` + seq counter) — UI 스레드의 `updateConfig`가 오디오 콜백을 절대 블록하지 않음 |
| **Click/zipper 방지** | 64~512 sample 선형 ramp + 종점 snap. instant swap 대비 click 폭 ~64×↓ |
| **Denormal CPU 폭주 방지** | `process()` 진입 시 RAII로 FZ/DAZ 활성, 종료 시 복원 (arm64/armv7/x86 분기) |
| **Stereo image 보존** | Limiter가 매 프레임 `max(|L|,|R|)` 기준으로 동일 게인 적용 |
| **Lifecycle UAF 차단** | Atomic engine pointer + in-flight counter drain pattern. `init/release/updateConfig/processDirectBuffer/computeAutoPreampDB` **모든 경로** 보호 |
| **Data race 부재** | `computeAutoPreampDB`가 audio thread 활성 상태 대신 config-side snapshot을 mutex로 보호 |
| **부분 config = 진짜 bypass** | `bands.size() < 20`이면 나머지 필터는 자동으로 unity 계수 (stale 계수 잔존 0) |
| **입력 sanitization** | NaN/Inf/Q≤0/지원 범위 밖 sampleRate 모두 안전한 fallback (flat coeff 또는 48kHz 기본값) |
| **PCM finite guard** | 입력 PCM NaN/Inf는 cascade 진입 전 0으로 mute. cascade 출력이 non-finite면 20개 biquad delay state를 reset해 IIR 영구 오염 방지 |

### 🔌 JNI Hardening

| 항목 | 동작 |
|---|---|
| 배열 길이 검증 | 4-array (`types`/`freq`/`gain`/`Q`) min-clamp + 20-band cap |
| Direct ByteBuffer 검증 | `GetDirectBufferAddress` null + `GetDirectBufferCapacity` ≥ `numFrames*2*sizeof(float)` |
| `numFrames` 검증 | `≤ 0` early return |
| 패키지 독립성 | `EQ_JNI_CLASS_PATH` 매크로 — 빌드 시 `-D`로 변경 가능 |
| Symbol fallback | RegisterNatives + 레거시 `Java_com_example_audio_AudioEngineJNI_*` C 심볼 양쪽 export |

### 🧰 Wrapper Layer (Eq20BandInput)

UI/JNI에서 고정 길이 20개 슬라이더 배열을 다룰 때 사용:

| 필드 | 타입 | 기본값 | 비고 |
|---|---|---|---|
| `gainMin / gainMax` | `float` | `-12.0 / +12.0` | core adapter clamp 정책 (UI 제약 아님) |
| `qMin / qMax` | `float` | `0.05 / 10.0` | Q (or shelf S) clamp |
| `gains[20]` | `float[]` | `0.0` | dB |
| `qFactors[20]` | `float[]` | `0.707` | Q 또는 shelf S |
| `frequencies[20]` | `float[]` | 31.25Hz~20kHz sqrt(2) 그리드 | Hz |
| `filterTypes[20]` | `EqFilterType[]` | `PEAKING` × 20 | |
| `preampDB` | `float` | `0.0` | dB |
| `enableSoftLimiter` | `bool` | `true` | safer default |

`makeEqEngineConfig(Eq20BandInput)` 또는 `engine.updateConfig(Eq20BandInput)` 직접 호출.

---

## 📋 Prerequisites

| 항목 | 권장 버전 |
|---|---|
| Android NDK | r25 이상 (테스트: r27.0.12077973) |
| CMake | 3.22 이상 |
| Android API | 21 이상 (테스트: 24) |
| C++ 표준 | C++17 |

호스트(macOS/Linux) 빌드는 `clang++ -std=c++17`만 있으면 standalone harness까지 가능 (JNI 부분은 NDK 필요).

---

## 🚀 Installation

### 1) 소스 통합

`src/main/cpp/` 9개 파일을 안드로이드 프로젝트에 복사:

```bash
cp -r src/main/cpp/* /path/to/your-android-app/app/src/main/cpp/
```

### 2) 빌드 옵션 (CMakeLists.txt에 이미 포함됨)

| 옵션 | 기본값 | 설명 |
|---|---|---|
| `BUILD_JNI` | `ON` | `audioengine_jni` shared library (Android용) 빌드 |
| `BUILD_STANDALONE_TEST` | `OFF` | `standalone_test` executable (호스트 회귀 테스트) 빌드 |
| `EQ_JNI_CLASS_PATH` (cppFlags) | `com/example/audio/AudioEngineJNI` | RegisterNatives FindClass 경로 |

### 3) `app/build.gradle`

```groovy
android {
    defaultConfig {
        externalNativeBuild {
            cmake { cppFlags "-std=c++17" }
        }
    }
    externalNativeBuild {
        cmake { path "src/main/cpp/CMakeLists.txt" }
    }
}
```

> 💡 **다른 패키지/클래스명 사용 시**: `cppFlags '-std=c++17 -DEQ_JNI_CLASS_PATH=\"com/your/pkg/YourClass\"'`

---

## ⚡ Quick Start

### Kotlin

```kotlin
package com.example.audio

import java.nio.ByteBuffer
import java.nio.ByteOrder

class AudioEngineJNI {
    init { System.loadLibrary("audioengine_jni") }

    external fun init(sampleRate: Int)
    external fun release()
    external fun updateConfig(
        preampDB: Float,
        enableLimiter: Boolean,
        filterTypes: IntArray,    // 0=PEAKING, 1=LOW_SHELF, 2=HIGH_SHELF
        frequencies: FloatArray,  // Hz
        gains: FloatArray,        // dB
        qFactors: FloatArray      // Q (peaking) or S (shelf)
    )
    external fun processDirectBuffer(buffer: ByteBuffer, numFrames: Int)
    external fun computeAutoPreampDB(): Float
}

// --- 사용 예 ---
val actualPcmSampleRate = 48000 // decoder/AudioTrack/engine 모두 동일해야 함
val engine = AudioEngineJNI()
engine.init(actualPcmSampleRate)

engine.updateConfig(
    preampDB       = 0f,
    enableLimiter  = true,
    filterTypes    = intArrayOf(1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                0, 0, 0, 0, 0, 0, 0, 0, 0, 2),
    frequencies    = floatArrayOf(31.25f, 62.5f, 125f, 250f, 500f, 1000f, 2000f, 4000f, 8000f, 16000f,
                                  44.2f, 88.4f, 176.8f, 353.6f, 707.1f, 1414.2f, 2828.4f, 5656.9f, 11313.7f, 20000f),
    gains          = FloatArray(20) { 0f },
    qFactors       = FloatArray(20) { 0.707f }
)

// 권장 preamp 자동 산출 → 재적용 패턴
val autoPreamp = engine.computeAutoPreampDB()  // ex: -3.5f for boost-heavy preset
// (필요 시 동일 파라미터 + preampDB = autoPreamp 로 updateConfig 재호출)

// 오디오 콜백
val buffer = ByteBuffer.allocateDirect(numFrames * 2 * 4).order(ByteOrder.nativeOrder())
// ... fill buffer with PCM Float32 stereo interleaved ...
engine.processDirectBuffer(buffer, numFrames)
```

### Native C++ (직접 사용)

```cpp
#include "FineTuneEQEngine.h"

const double actualPcmSampleRate = 48000.0; // input/output PCM과 동일해야 함
FineTuneEQEngine eq(actualPcmSampleRate);

// 방법 A: EqEngineConfig (가변 길이 vector)
EqEngineConfig cfg;
cfg.preampDB          = 0.0f;
cfg.enableSoftLimiter = true;

EqBandConfig bass;
bass.type      = EqFilterType::LOW_SHELF;
bass.frequency = 60.0f;
bass.gainDB    = 4.0f;
bass.qFactor   = 0.707f;     // shelf S
cfg.bands.push_back(bass);

EqBandConfig presence;
presence.type      = EqFilterType::PEAKING;
presence.frequency = 4000.0f;
presence.gainDB    = 2.0f;
presence.qFactor   = 1.4f;   // peaking Q
cfg.bands.push_back(presence);

eq.updateConfig(cfg);

// 방법 B: Eq20BandInput (고정 20개 + clamp 정책)
Eq20BandInput input;       // 기본값으로 초기화 (flat 20-band)
input.gainMin = -8.0f;
input.gainMax =  8.0f;
input.gains[5] = 12.0f;    // → 8.0f로 자동 clamp
eq.updateConfig(input);

float pcm[1024 * 2];  // L,R,L,R...
eq.process(pcm, pcm, 1024);   // in-place 처리
```

전체 Kotlin 통합 가이드 → [DEVELOPER_GUIDE.md](./DEVELOPER_GUIDE.md)

---

## 📁 Project Structure

```text
AndroidEQModule/
├── README.md                          # 본 문서
├── DEVELOPER_GUIDE.md                 # Kotlin 연동 / 트러블슈팅 / 패키지 변경 가이드
└── src/main/cpp/
    ├── BiquadFilter.h / .cpp          # Stateful TDF-II Biquad + lock-free seqlock + sample-accurate ramp
    ├── FineTuneEQEngine.h / .cpp      # Pipeline (Preamp → 20-band cascade fusion → Soft Limiter)
    │                                  # + Eq20BandInput wrapper + makeEqEngineConfig adapter
    ├── AudioEngineJNI.cpp             # JNI bridge (lifecycle drain + 입력 검증 + RegisterNatives)
    ├── RTSafetyUtils.h                # FZ/DAZ helpers (arm64/armv7/x86), RT-safe markers
    ├── DenormalGuard.h                # RAII denormal protection
    ├── standalone_test.cpp            # 호스트 회귀 테스트 (BUILD_STANDALONE_TEST=ON 시)
    └── CMakeLists.txt                 # eq_core (static) + audioengine_jni (shared) + standalone_test
```

---

## 📖 API Reference

### `FineTuneEQEngine` (C++ Native API)

| 메서드 | 시그니처 | 호출 스레드 | 설명 |
|---|---|---|---|
| ctor | `FineTuneEQEngine(double sampleRate)` | UI/init | `[44100, 384000]` 외 입력은 48000으로 fallback |
| `updateConfig` | `void updateConfig(const EqEngineConfig&)` | UI | 가변 길이 vector. NOT_RT_SAFE |
| `updateConfig` | `void updateConfig(const Eq20BandInput&)` | UI | 고정 20개 wrapper. clamp 정책 자동 적용 |
| `process` | `void process(const float* in, float* out, int numFrames)` | **오디오 콜백** | RT-safe. `in == out` (in-place) 허용 |
| `computeAutoPreampDB` | `float computeAutoPreampDB() const` | UI | 캐스케이드 누적 게인 기반 권장 preamp dB. snapshot 기반 (race-free) |

### `EqBandConfig` 기본값

| 필드 | 타입 | 기본값 | 비고 |
|---|---|---|---|
| `type` | `EqFilterType` | `PEAKING` | `PEAKING` / `LOW_SHELF` / `HIGH_SHELF` |
| `frequency` | `float` | `1000.0f` | Hz, `(0, sampleRate/2)` |
| `gainDB` | `float` | `0.0f` | dB, NaN/Inf → unity coeffs |
| `qFactor` | `float` | `0.707f` | PEAKING은 Q, LOW/HIGH_SHELF는 shelf S. `< 0.05`은 0.05로 클램프 |

### `EqEngineConfig` 기본값

| 필드 | 타입 | 기본값 | 비고 |
|---|---|---|---|
| `preampDB` | `float` | `0.0f` | dB, NaN/Inf → 0dB로 fallback |
| `enableSoftLimiter` | `bool` | `true` | safer default |
| `bands` | `vector<EqBandConfig>` | `{}` | 0~20개 가능. 빈 vector = 완전 bypass |

### `Eq20BandInput` 기본값

기본 생성 시 31.25Hz ~ 20kHz `sqrt(2)` 그리드, 모든 gain=0dB, 모든 Q=0.707, 모든 type=PEAKING의 **flat preset**.

### JNI 진입점 (Kotlin → C++)

| C 심볼 | Java 시그니처 | 호출 스레드 |
|---|---|---|
| `Java_com_example_audio_AudioEngineJNI_init` | `(I)V` | UI |
| `Java_com_example_audio_AudioEngineJNI_release` | `()V` | UI |
| `Java_com_example_audio_AudioEngineJNI_updateConfig` | `(FZ[I[F[F[F)V` | UI |
| `Java_com_example_audio_AudioEngineJNI_processDirectBuffer` | `(Ljava/nio/ByteBuffer;I)V` | **오디오 콜백** |
| `Java_com_example_audio_AudioEngineJNI_computeAutoPreampDB` | `()F` | UI |
| `JNI_OnLoad` | `(JavaVM*, void*) → jint` | 시스템 |

---

## ✅ Build & ABI Matrix

NDK r27 / API-24 기준 빌드 결과 (Release):

| ABI | `libaudioengine_jni.so` |
|---|---|
| `arm64-v8a` | ✅ ~811 KB |
| `armeabi-v7a` | ✅ ~613 KB |
| `x86_64` | ✅ ~734 KB |

### 호스트 standalone 빌드 (회귀 테스트)

```bash
cmake -S src/main/cpp -B build/host \
  -DBUILD_STANDALONE_TEST=ON \
  -DBUILD_JNI=OFF \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/host -j
./build/host/standalone_test
# Expected: PASS: standalone regression harness
```

### Android NDK 빌드

```bash
NDK=/path/to/Android/sdk/ndk/27.0.12077973
cmake -S src/main/cpp -B build/android-arm64 \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-24 \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/android-arm64 -j
```

---

## 🧪 Verification & Quality Gates

`standalone_test` 12개 테스트가 핵심 동작을 회귀 검증합니다:

| # | 테스트 | 검증 항목 |
|---|---|---|
| 1 | `pipelineSmoke` | 20-band 풀 캐스케이드 + 리미터 처리 후 NaN/Inf 0건, peak ≤ 1.01 |
| 2 | `emptyConfigRemovesStaleFilters` | +12dB 적용 후 빈 config 적용 → 출력이 입력과 동일 (max diff < 1e-3) |
| 3 | `invalidParameterSafety` | NaN sampleRate, NaN preamp, NaN/Inf/음수 freq·Q → 출력 NaN 0건, peak ≤ 1.01 |
| 4 | `nonFinitePcmInputDoesNotPoisonState` | PCM NaN/Inf 1샘플 입력 → 출력 finite 유지, 다음 정상 PCM block도 finite |
| 5 | `nonFiniteCascadeOutputResetsState` | 극단적 finite PCM으로 내부 overflow 발생 가능 시 출력 mute + biquad state reset 후 정상 block 복구 |
| 6 | `inPlaceOutOfPlaceEquivalence` | `process(buf,buf)` ≡ `process(in,out)` (max diff < 1e-6) |
| 7 | `autoPreampImmediateForPlus12dB` | `updateConfig` 직후 `computeAutoPreampDB()` 반환값이 `[-13.5, -10.5] dB` 범위 |
| 8 | `wrapperClampAndEquivalence` | `Eq20BandInput`의 gainMin/Max·qMin/Max clamp + `updateConfig(Eq20BandInput) ≡ updateConfig(makeEqEngineConfig(input))` |
| 9 | `sampleRateSupportMatrix` | 44.1k/48k/88.2k/96k/176.4k/192k/352.8k/384kHz에서 finite output + finite auto preamp |
| 10 | `unsupportedSampleRateFallsBackTo48000` | 44099/384001/NaN/Inf/0/-1Hz 입력이 48kHz fallback 출력과 동일 |
| 11 | `sameRateNoResamplingFrameInvariant` | 44100/48000/384000Hz에서 `numFrames`로 지정한 stereo sample 영역만 기록 |
| 12 | `nyquistFrequencyPolicy` | 44.1kHz에서 20kHz band 유효, 23kHz band flat 처리, 384kHz에서 100kHz band finite |

### Sanitizer 결과

| Sanitizer | 결과 |
|---|---|
| TSan (data race) | ✅ Clean |
| ASan (memory error) | ✅ Clean |
| UBSan (undefined behavior) | ✅ Clean |

---

## 🚦 Production Readiness

이 모듈은 다음 프로덕션 시나리오에서 발생하는 실제 폭발 케이스를 통과한 상태입니다.

| 프로덕션 시나리오 | 차단 메커니즘 |
|---|---|
| 재생 중 EQ 슬라이더 빠른 드래그 | Lock-free seqlock + 64~512 sample ramp |
| 화면 종료 직전 `release()` 호출 | ProcessGuard drain pattern (in-flight counter) |
| 백그라운드 ↔ 포그라운드 빠른 토글 | `init/release` 사이의 swap & drain |
| 자동 preamp UI 갱신과 슬라이더 변경 동시 발생 | Snapshot mutex (audio thread 활성 상태와 분리) |
| 프리셋 초기화 (`bands` 비우기 / 부분 적용) | NUM_BANDS 전체 루프 + flat fallback |
| 사용자 입력 부주의 (NaN/Inf gain·Q, 0 Q, 음수 freq) | sanitizeBandInputs → flat coeff fallback |
| 잘못된 sampleRate 전달 | 44100~384000 외 → 48000 fallback |
| 업스트림 PCM NaN/Inf 유입 | sample-level finite guard → 해당 sample mute + 필요 시 biquad delay state reset |
| Kotlin 측 잘못된 배열 길이 / null / 작은 ByteBuffer | JNI 입력 검증 + early return + audit log |
| 패키지명 변경 | `EQ_JNI_CLASS_PATH` 매크로 (소스 수정 불필요) |

### 프로덕션 출고 전 체크리스트

- [x] DSP 수식 정확성 (RBJ Cookbook 100% 일치)
- [x] Real-Time 안전 계약 (lock-free, allocation-free, blocking-free 오디오 콜백)
- [x] Lifecycle UAF 차단 (init/release/updateConfig/processDirectBuffer/computeAutoPreampDB 5개 진입점)
- [x] Data race 부재 (TSan clean)
- [x] Memory safety (ASan clean)
- [x] Undefined behavior 부재 (UBSan clean)
- [x] 입력 sanitization (NaN/Inf/범위 외 모두 안전한 fallback)
- [x] PCM finite guard (NaN/Inf sample mute + IIR state recovery)
- [x] Click/zipper 방지 (sample-accurate coefficient ramp)
- [x] Denormal CPU 폭주 방지 (FZ/DAZ RAII guard)
- [x] Stereo image 보존 (linked limiter)
- [x] NDK 멀티 ABI 빌드 (arm64-v8a / armeabi-v7a / x86_64 × Release / Debug)
- [x] 호스트 standalone 회귀 (12 케이스, sanitizer 동시 통과)

---

## 🎚️ DSP 사양

| 항목 | 값 |
|---|---|
| 채널 | Stereo interleaved (L, R, L, R…) |
| 샘플 포맷 | Float32 (`-1.0 ~ +1.0`) |
| 지원 sampleRate | 44100 ~ 384000 Hz (그 외 입력은 48000으로 fallback) |
| sampleRate 처리 | Same-rate DSP only. 입력 PCM sampleRate = engine sampleRate = 출력 PCM sampleRate. Resampling 없음 |
| 밴드 수 | 20 (고정) |
| Q / S 범위 | 실용 0.05 ~ 100+ (0.05 미만은 0.05로 클램프) |
| Gain 범위 | 무제한 (NaN/Inf만 차단; preamp 자동 산출 권장) |
| PCM finite guard | 입력 PCM NaN/Inf는 0으로 mute. cascade 출력 non-finite 감지 시 20-band biquad delay state reset |
| Coefficient ramp | 64~512 samples (`fs / 750` 기반 자동 결정, 기본 64@48k) |
| Soft Limiter | Asymptotic soft-knee, threshold 0.95, headroom 0.05, stereo-linked |
| Preamp 산출 grid | 256 log-spaced points, `[20Hz, fs*0.475]` |

---

## 🔧 Troubleshooting

자세한 트러블슈팅은 [DEVELOPER_GUIDE.md](./DEVELOPER_GUIDE.md)를 참조하세요.

| 증상 | 빠른 진단 |
|---|---|
| 소리가 찢어짐 (clipping) | `AudioFormat.ENCODING_PCM_FLOAT` 사용? `computeAutoPreampDB()` 적용? |
| EQ 중심 주파수가 어긋남 | decoder/AudioTrack 실제 sampleRate와 `engine.init(sampleRate)` 값이 같은지 확인 |
| 슬라이더 드래그 시 click | C++ 엔진을 매번 재생성하지 않는지 확인 — `updateConfig`만 다시 호출 |
| `UnsatisfiedLinkError` | Kotlin 클래스명이 `AudioEngineJNI`인지 확인. 또는 `-DEQ_JNI_CLASS_PATH=...` 빌드 옵션 |
| `init/release` 토글 시 크래시 | 현재 버전은 ProcessGuard drain pattern으로 차단됨. 이전 버전이면 업데이트 |
| logcat에 `[AudioEngineJNI]` 에러 | JNI 입력 검증 실패 — 배열 길이/null/buffer capacity 확인 |
| Shelf 필터의 `qFactor` 동작이 예상과 다름 | LOW_SHELF/HIGH_SHELF에선 `qFactor`가 RBJ shelf slope S로 해석됨 (Q 아님) |

---

## 🗺️ Roadmap

본 모듈에서 의도적으로 제외된 항목 — 필요 시 후속 모듈로:

- [ ] DF-I 또는 SVF 형식 변형 (고-Q 분해능 향상)
- [ ] Lookahead peak limiter (현재는 instantaneous soft limiter)
- [ ] Limiter 2× oversampling (alias 디스토션 감소)
- [ ] Frequency pre-warping (RBJ Nyquist boundary 보정)
- [ ] Linear-phase / Dynamic EQ 모드 (별도 모듈)

---

## 🤝 Contributing

이슈/PR 환영합니다.

1. 이슈 등록 — 재현 가능한 케이스
2. 브랜치 — `feature/xxx` 또는 `fix/xxx`
3. 변경 검증 — 호스트 standalone 회귀 (`-DBUILD_STANDALONE_TEST=ON`) + NDK 빌드
4. PR — 변경 의도와 영향 범위 명시

스타일 규칙: 외부 시그니처(JNI, `EqEngineConfig`, `Eq20BandInput`, `process`) 변경 금지. 내부 구현은 자유.

---

## 📜 License

본 프로젝트의 라이선스는 별도 협의 — 사용 전 [coreline-ai](https://github.com/coreline-ai)에 문의해 주세요.

---

<div align="center">

**Made with ⚡ by Coreline · Powered by [RBJ Audio EQ Cookbook](https://webaudio.github.io/Audio-EQ-Cookbook/audio-eq-cookbook.html)**

</div>
