<div align="center">

# 🎧 Android 20-Band Parametric EQ Module

[![C++](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white)](https://en.cppreference.com/w/cpp/17)
[![Android NDK](https://img.shields.io/badge/Android%20NDK-r27-3DDC84?logo=android&logoColor=white)](https://developer.android.com/ndk)
[![CMake](https://img.shields.io/badge/CMake-3.22%2B-064F8C?logo=cmake&logoColor=white)](https://cmake.org/)
[![ABIs](https://img.shields.io/badge/ABIs-arm64--v8a%20%7C%20armv7a%20%7C%20x86__64-blue)](#-build--abi-matrix)
[![DSP](https://img.shields.io/badge/DSP-RBJ%20Cookbook-orange)](https://webaudio.github.io/Audio-EQ-Cookbook/audio-eq-cookbook.html)
[![Real-Time Safe](https://img.shields.io/badge/Real--Time-Lock--Free-green)](#%EF%B8%8F-real-time-safety-guarantees)
[![Build](https://img.shields.io/badge/Build-6%2F6%20OK-brightgreen)](#-build--abi-matrix)
[![License](https://img.shields.io/badge/License-Proprietary-lightgrey)](#-license)

**스튜디오급 20-band Parametric EQ — Android에서 lock-free, real-time safe로 동작하는 독립형 C++ DSP 코어**

RBJ Audio EQ Cookbook 표준 수식 · 스테레오 인터리브 Float32 · JNI Zero-Copy · Oboe/AAudio/AudioTrack 어떤 I/O와도 결합 가능

[Overview](#-overview) · [Features](#-features) · [Quick Start](#-quick-start) · [API](#-api-reference) · [Build](#-build--abi-matrix) · [Troubleshooting](./DEVELOPER_GUIDE.md)

</div>

---

## 🎯 Overview

이 모듈은 안드로이드(또는 일반 C++) 환경에서 오디오 스트림에 **20-band Parametric EQ**를 적용하는 독립형 C++ DSP 코어입니다. 오디오 I/O 계층(Oboe/AAudio/AudioTrack)과 DSP 연산을 완전히 분리(Decoupling)한 블랙박스로 설계되어, 어떤 출력 파이프라인에 끼워 넣어도 동일하게 동작합니다.

### 신호 흐름

```text
   ┌──────────┐   ┌────────────────────────────┐   ┌─────────────────┐
   │  Preamp  │──▶│ 20-Band Biquad Cascade     │──▶│  Soft Limiter   │──▶ Output
   │  (gain)  │   │ (Stateful TDF-II, lock-free│   │ (stereo-linked) │   PCM
   └──────────┘   │  coefficient publication)  │   └─────────────────┘   Float32
        ▲         └────────────────────────────┘            ▲
        │                       ▲                           │
        │                       │                           │
   ┌────┴───────────────────────┴───────────────────────────┴──────┐
   │   updateConfig() — UI/JNI thread (lock-free, click-free ramp) │
   └───────────────────────────────────────────────────────────────┘
```

| 단계 | 역할 |
|---|---|
| **Preamp** | 누적 캐스케이드 최대 게인을 상쇄해 헤드룸 확보 — `computeAutoPreampDB()`가 분석적 \|H(f)\| 그리드 평가로 권장값 자동 산출 |
| **20-Band Cascade** | 샘플 단위 `processSampleL/R` 직렬 통과 (cascade fusion). 계수는 lock-free seqlock 슬롯으로 발행, 64~512 sample 선형 ramp |
| **Soft Limiter** | 스테레오 링크 (양 채널 동일 게인)로 이미지 보존하면서 0 dBFS 초과 차단 |

---

## 📦 Features

### 🎛️ 지원 필터 타입

| 타입 | 용도 | 파라미터 |
|---|---|---|
| `PEAKING` (Bell) | 특정 대역 boost / cut | freq, gain, Q |
| `LOW_SHELF` | 저역 일괄 boost / cut | freq, gain, Q |
| `HIGH_SHELF` | 고역 일괄 boost / cut | freq, gain, Q |

수식은 [RBJ Audio EQ Cookbook](https://webaudio.github.io/Audio-EQ-Cookbook/audio-eq-cookbook.html)을 100% 따르며, DAW(Cubase, Logic, Reaper) 및 EqualizerAPO와 동일한 결과를 산출합니다.

### 🛡️ Real-Time Safety Guarantees

| 보장 | 메커니즘 |
|---|---|
| **Lock-free coefficient publication** | Seqlock-style atomic 슬롯 — UI 스레드의 `updateConfig`가 오디오 콜백을 절대 블록하지 않음 |
| **Click/zipper 방지** | 64~512 sample 선형 ramp + 종점 snap (instant swap 대비 64×↓) |
| **Denormal CPU 폭주 방지** | `process()` 진입 시 RAII로 FZ/DAZ 활성, 종료 시 복원 (arm64/armv7/x86 분기) |
| **Stereo image 보존** | Limiter가 L/R 동일 게인으로 동시 감쇠 |
| **Lifecycle UAF 차단** | Atomic engine pointer + in-flight counter drain pattern (init/release/updateConfig/computeAutoPreampDB 모든 경로 보호) |
| **Data race 부재** | `computeAutoPreampDB`가 audio thread 상태 대신 config-side snapshot을 mutex로 보호 (TSan clean) |
| **부분 config = 진짜 bypass** | `bands`가 비어있거나 N<20이면 나머지 필터는 자동으로 flat (stale 계수 잔존 0) |
| **입력 sanitization** | NaN/Inf, Q≤0, 잘못된 sampleRate 모두 안전한 fallback (flat coeff 또는 default) |

### 🔌 JNI Hardening

| 항목 | 동작 |
|---|---|
| 배열 길이 검증 | 4-array (`types`/`freq`/`gain`/`Q`) min-clamp + 20-band cap |
| Direct ByteBuffer 검증 | `GetDirectBufferAddress` null + `GetDirectBufferCapacity` ≥ `numFrames*2*sizeof(float)` |
| `numFrames` 검증 | `≤ 0` early return |
| 패키지 독립성 | `EQ_JNI_CLASS_PATH` 매크로로 빌드 시 변경 가능 (레거시 C 심볼 fallback 유지) |

---

## 📋 Prerequisites

| 항목 | 권장 버전 |
|---|---|
| Android NDK | r25 이상 (테스트: r27) |
| CMake | 3.22 이상 |
| Android API | 21 이상 (테스트: 24) |
| C++ 표준 | C++17 |

호스트(macOS/Linux) 빌드는 `clang++ -std=c++17`만 있으면 가능 (JNI 부분 제외 가능).

---

## 🚀 Installation

### 1) 소스 통합

`src/main/cpp/` 8개 파일을 안드로이드 프로젝트의 `app/src/main/cpp/`로 복사:

```bash
cp -r src/main/cpp/* /path/to/your-android-app/app/src/main/cpp/
```

### 2) `app/src/main/cpp/CMakeLists.txt` (이미 포함되어 있으면 스킵)

```cmake
cmake_minimum_required(VERSION 3.22.1)
project("AndroidEQModule")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(${ANDROID_ABI} STREQUAL "armeabi-v7a")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mfpu=neon")
endif()

add_library(audioengine_jni SHARED
    BiquadFilter.cpp
    FineTuneEQEngine.cpp
    AudioEngineJNI.cpp
)
target_link_libraries(audioengine_jni android log)
```

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

> 💡 **다른 패키지명을 쓰는 경우**: `cppFlags '-std=c++17 -DEQ_JNI_CLASS_PATH=\"com/your/pkg/YourClass\"'` 추가.

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
        frequencies: FloatArray,
        gains: FloatArray,
        qFactors: FloatArray
    )
    external fun processDirectBuffer(buffer: ByteBuffer, numFrames: Int)
    external fun computeAutoPreampDB(): Float
}

// --- 사용 예 ---
val engine = AudioEngineJNI()
engine.init(48000)

engine.updateConfig(
    preampDB       = 0f,
    enableLimiter  = true,
    filterTypes    = intArrayOf(1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2),
    frequencies    = floatArrayOf(60f, 120f, 250f, 500f, 800f, 1000f, 1500f, 2000f, 3000f, 4000f,
                                  5000f, 6000f, 7000f, 8000f, 9000f, 10000f, 12000f, 14000f, 15000f, 16000f),
    gains          = FloatArray(20) { 0f },
    qFactors       = FloatArray(20) { 0.707f }
)

val autoPreamp = engine.computeAutoPreampDB()  // ex: -3.5f for boost-heavy preset
// ... re-apply config with preampDB = autoPreamp ...

// 오디오 콜백에서:
val buffer = ByteBuffer.allocateDirect(numFrames * 2 * 4).order(ByteOrder.nativeOrder())
// ... fill buffer with PCM Float32 stereo interleaved ...
engine.processDirectBuffer(buffer, numFrames)
```

### Native C++

```cpp
#include "FineTuneEQEngine.h"

FineTuneEQEngine eq(48000.0);

EqEngineConfig cfg;
cfg.preampDB          = 0.0f;
cfg.enableSoftLimiter = true;

EqBandConfig bass;
bass.type      = EqFilterType::LOW_SHELF;
bass.frequency = 60.0f;
bass.gainDB    = 4.0f;
bass.qFactor   = 0.707f;
cfg.bands.push_back(bass);

EqBandConfig presence;
presence.type      = EqFilterType::PEAKING;
presence.frequency = 4000.0f;
presence.gainDB    = 2.0f;
presence.qFactor   = 1.4f;
cfg.bands.push_back(presence);

eq.updateConfig(cfg);

float pcm[1024 * 2];  // L,R,L,R...
eq.process(pcm, pcm, 1024);   // in-place
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
    ├── AudioEngineJNI.cpp             # JNI bridge (lifecycle drain + 입력 검증 + RegisterNatives)
    ├── RTSafetyUtils.h                # FZ/DAZ helpers (arm64/armv7/x86), RT-safe markers
    ├── DenormalGuard.h                # RAII denormal protection
    └── CMakeLists.txt                 # NDK 빌드
```

---

## 📖 API Reference

### `FineTuneEQEngine` (C++ Native API)

| 메서드 | 시그니처 | 호출 스레드 | 설명 |
|---|---|---|---|
| ctor | `FineTuneEQEngine(double sampleRate)` | UI/init | 8000~384000 Hz 외 입력은 48000으로 fallback |
| `updateConfig` | `void updateConfig(const EqEngineConfig&)` | UI | preamp/limiter on-off + 20밴드 계수 갱신 (NOT_RT_SAFE) |
| `process` | `void process(const float* in, float* out, int numFrames)` | **오디오 콜백** | RT-safe DSP. `in == out` (in-place) 허용 |
| `computeAutoPreampDB` | `float computeAutoPreampDB() const` | UI | 캐스케이드 누적 게인 기반 권장 preamp dB 반환 (snapshot 기반) |

### `EqBandConfig` 기본값

| 필드 | 타입 | 기본값 | 비고 |
|---|---|---|---|
| `type` | `EqFilterType` | `PEAKING` | `PEAKING` / `LOW_SHELF` / `HIGH_SHELF` |
| `frequency` | `float` | `1000.0f` | Hz, `(0, sampleRate/2)` |
| `gainDB` | `float` | `0.0f` | dB, NaN/Inf → flat |
| `qFactor` | `float` | `0.707f` | Butterworth, `< 0.05`은 0.05로 클램프 |

### `EqEngineConfig` 기본값

| 필드 | 타입 | 기본값 | 비고 |
|---|---|---|---|
| `preampDB` | `float` | `0.0f` | dB, NaN/Inf → 0dB로 fallback |
| `enableSoftLimiter` | `bool` | `true` | safer default |
| `bands` | `vector<EqBandConfig>` | `{}` | 0~20개 가능. 빈 vector = 완전 bypass |

### JNI 진입점 (Kotlin → C++)

| C 심볼 | Java 시그니처 | 호출 스레드 |
|---|---|---|
| `Java_com_example_audio_AudioEngineJNI_init` | `(I)V` | UI |
| `Java_com_example_audio_AudioEngineJNI_release` | `()V` | UI |
| `Java_com_example_audio_AudioEngineJNI_updateConfig` | `(FZ[I[F[F[F)V` | UI |
| `Java_com_example_audio_AudioEngineJNI_processDirectBuffer` | `(Ljava/nio/ByteBuffer;I)V` | **오디오 콜백** |
| `Java_com_example_audio_AudioEngineJNI_computeAutoPreampDB` | `()F` | UI |
| `JNI_OnLoad` | — | 시스템 |

---

## ✅ Build & ABI Matrix

NDK r27 / API-24 기준 빌드 결과:

| ABI | Release | Debug |
|---|---|---|
| `arm64-v8a` | ✅ ~800 KB | ✅ ~966 KB |
| `armeabi-v7a` | ✅ ~600 KB | ✅ ~744 KB |
| `x86_64` | ✅ ~725 KB | ✅ ~880 KB |

```bash
# 호스트 컴파일 검증 (clang++)
cd src/main/cpp
clang++ -std=c++17 -O2 -Wall -Wextra -Werror -c BiquadFilter.cpp
clang++ -std=c++17 -O2 -Wall -Wextra -Werror -c FineTuneEQEngine.cpp

# Android NDK 빌드
NDK=/path/to/Android/sdk/ndk/27.0.12077973
cmake -S src/main/cpp -B build \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

---

## 🎚️ DSP 사양

### 동작 사양

| 항목 | 값 |
|---|---|
| 채널 | Stereo interleaved (L, R, L, R…) |
| 샘플 포맷 | Float32 (`-1.0 ~ +1.0`) |
| 지원 sampleRate | 8000 ~ 384000 Hz (그 외 48000 fallback) |
| 밴드 수 | 20 (고정) |
| Q 범위 | 실용 0.05 ~ 100+ (0.05 미만은 0.05로 클램프) |
| Gain 범위 | 무제한 (NaN/Inf만 차단; preamp 자동 산출 권장) |
| 처리량 (Apple M-series 기준) | ~36 MFrames/s, 48kHz 기준 약 **750× 실시간** |
| Coefficient ramp | 64~512 samples (`fs / 750` 기반 자동 결정) |
| Limiter | Soft-knee asymptotic, threshold 0.95, headroom 0.05, stereo-linked |

### 검증 결과

| 항목 | 결과 |
|---|---|
| Cascade fusion vs 20-pass reference | **3.7~4.2× speedup**, 비트-동등 |
| Coefficient ramp click 억제 | instant swap 대비 **64×↓** |
| Auto preamp 추정 정확도 | +12dB peak → **-11.998 dB** (오차 ±0.002 dB) |
| Stereo-linked limiter | L/R gain diff = **0.000** |
| Denormal 처리 | -160 dBFS 입력 정상 신호 대비 **1.07×** |
| TSan / ASan / UBSan | **0 race / 0 UAF / 0 UB** |

---

## 🔧 Troubleshooting

자주 묻는 질문은 [DEVELOPER_GUIDE.md](./DEVELOPER_GUIDE.md)에 정리되어 있습니다.

| 증상 | 빠른 진단 |
|---|---|
| 소리가 찢어짐 (clipping) | `AudioFormat.ENCODING_PCM_FLOAT` 사용? `computeAutoPreampDB()` 적용? |
| 슬라이더 드래그 시 click | C++ 엔진을 재생성 중인지 확인 — `updateConfig`만 다시 호출 |
| `UnsatisfiedLinkError` | Kotlin 클래스명이 `AudioEngineJNI`인지 확인 또는 `-DEQ_JNI_CLASS_PATH=...` |
| `init/release` 토글 시 크래시 | 현재 버전은 ProcessGuard drain pattern으로 방지됨 |
| logcat에 `[AudioEngineJNI]` 에러 | JNI 입력 검증 실패 메시지 — 배열 길이/null/buffer capacity 확인 |

---

## 🗺️ Roadmap

본 모듈에서 의도적으로 제외된 항목 — 필요 시 후속 작업으로 진행:

- [ ] DF-I 또는 SVF 형식 변형 (고-Q 분해능 향상)
- [ ] Lookahead peak limiter (현재는 instantaneous soft limiter)
- [ ] Limiter 2× oversampling (alias 디스토션 감소)
- [ ] Frequency pre-warping (RBJ Nyquist boundary 보정)
- [ ] Linear-phase / Dynamic EQ 모드

---

## 🤝 Contributing

이슈/PR 환영합니다.

1. 이슈 등록 — 재현 가능한 케이스
2. 브랜치 생성 — `feature/xxx` 또는 `fix/xxx`
3. 변경 사항 테스트 — 가능하면 호스트 측 빌드 (`-std=c++17 -Wall -Wextra -Werror`)
4. PR — 변경 의도와 영향 범위 명시

스타일 규칙: 외부 시그니처(JNI, `EqEngineConfig`, `process`) 변경 금지. 내부 구현은 자유.

---

## 📜 License

본 프로젝트의 라이선스는 별도 협의 — 사용 전 [coreline-ai](https://github.com/coreline-ai)에 문의해 주세요.

---

<div align="center">

**Made with ⚡ by Coreline · Powered by RBJ Audio EQ Cookbook**

</div>
