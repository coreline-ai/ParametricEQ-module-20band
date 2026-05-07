<div align="center">

# 🎧 Android 20-Band Parametric EQ Module

**Android/Kotlin에서 호출 가능한 20-band Parametric EQ C++ DSP 코어**

RBJ Audio EQ Cookbook 기반 · Stereo interleaved Float32 · JNI Direct ByteBuffer 처리 · Parametric EQ 전용

[API](#-api--input-contract) · [Wrapper 목표](#-wrapper-input-structure-target) · [Build](#-build--verification) · [Developer Guide](./DEVELOPER_GUIDE.md)

</div>

---

## 🎯 Scope

이 모듈의 범위는 **20-band Parametric EQ**입니다.

- 포함: `PEAKING`, `LOW_SHELF`, `HIGH_SHELF` 기반 biquad cascade, preamp, soft limiter, JNI bridge
- 제외: Linear-phase EQ, Dynamic EQ, Oboe player 구현, Android UI 구현
- Linear-phase/Dynamic EQ는 비교 대상 또는 제외 범위로만 언급하며, 이 문서의 구현 계획에 포함하지 않습니다.

---

## 📊 EQ Type Comparison

본 모듈이 **왜 Parametric EQ만 포함하고 다른 두 종류는 제외했는지** 결정 근거를 한 표로 정리합니다.

| 구분 | Parametric EQ | Linear-phase EQ | Dynamic EQ |
|---|---|---|---|
| **기본 개념** | 특정 주파수 대역을 고정적으로 올리거나 내림 | 주파수 보정 시 위상 왜곡을 최소화 | 특정 대역의 소리 크기에 따라 EQ가 자동으로 움직임 |
| **동작 방식** | `frequency`, `gain`, `Q`로 IIR biquad 적용 (RBJ Cookbook) | FIR / FFT / convolution 기반 선형 위상 필터 | EQ + compressor 결합. threshold·attack·release 기반 |
| **Gain 변화** | 항상 고정 (정적) | 항상 고정 (정적) | 입력 신호 레벨에 따라 실시간 변화 |
| **Phase 영향** | 있음 (IIR 필연) | 거의 없음 (linear-phase) | 일반적으로 있음 |
| **Latency** | 낮음 (수 sample) | 높음 (수십~수백 ms, FIR 길이 비례) | 낮음~중간 (envelope follower 추가) |
| **CPU 비용** | 낮음 | 높음 (FFT/convolution) | 중간~높음 |
| **Pre-ringing** | 없음 | 있음 (transient에서 가청) | 없음 |
| **장점** | 빠르고 실시간 처리에 적합, 구현 단순 | 위상 보존 → 믹싱/마스터링 phase coherence | 문제 대역만 자동 제어, 자연스러운 보정 |
| **단점** | 위상 변화 발생 | latency / CPU / pre-ringing | 구현 및 튜닝 복잡, dynamics 부작용 |
| **대표 용도** | 음악 앱 EQ, 헤드폰 보정, 톤 조절 | 스튜디오 마스터링, 멀티트랙 phase alignment | 디에서, 보컬/저역 제어, 자동 보정 |
| **Android 실시간 재생 적합성** | 🟢 높음 | 🔴 낮음 (latency·CPU 부담) | 🟡 중간 |
| **본 프로젝트 포함 여부** | ✅ **포함 (구현 완료)** | ❌ 제외 (out of scope) | ❌ 제외 (out of scope) |

### 본 프로젝트가 Parametric EQ를 선택한 이유

| 기준 | 결정 근거 |
|---|---|
| **Latency** | Android 실시간 재생 콜백은 10~20 ms 버퍼 단위. Linear-phase의 수십 ms latency는 글리치/입출력 위치 미스매치를 유발 |
| **CPU 예산** | 모바일 SoC에서 FFT/convolution 기반 EQ는 발열·배터리 부담. IIR biquad 20개는 약 750× 실시간 처리 (Apple M-series 호스트 기준) |
| **Phase coherence 요구도** | 음악 앱 / 헤드폰 보정에서 위상 일치는 마스터링급 요구가 아님. Pre-ringing 없는 IIR이 일반 청취 환경에 더 적합 |
| **결정성** | 정적 EQ (사용자 설정에 따른 고정 응답)이 디버깅·튜닝·재현성 모두 단순 |

> 📌 **확장 시점에 대한 노트**: 만약 향후 마스터링 모드, 자동 디에서, 또는 시끄러운 환경 적응 보정이 필요하다면 별도 모듈로 Linear-phase EQ 또는 Dynamic EQ를 추가할 수 있으나, 본 모듈의 시그니처 (`updateConfig`, `process`, `EqEngineConfig`)는 변경하지 않는 방향이 호환성에 안전합니다.

---

## 🔊 Signal Flow

```text
Input Float32 stereo PCM
   → Preamp
   → 20-band Parametric EQ biquad cascade
   → optional Soft Limiter
   → Output Float32 stereo PCM
```

`Preamp`는 “가장 큰 슬라이더 gain만큼 단순 감쇠”가 아닙니다. 현재 config의 20개 band가 합쳐진 **누적 주파수 응답**을 `computeAutoPreampDB()`가 grid 기반으로 평가하고, cascade peak가 0 dBFS를 넘지 않도록 권장 preamp dB를 산출합니다. 순수 cut 위주의 preset이면 0 dB를 반환할 수 있습니다.

---

## 📋 API & Input Contract

### Current default input

현재 Kotlin/JNI 기본 설정 입력은 고정 길이 20개 배열입니다.

| 입력 | 의미 |
|---|---|
| `gains[20]` | band별 gain dB |
| `qFactors[20]` | band별 Q |
| `frequencies[20]` | band별 center/cutoff frequency Hz |
| `filterTypes[20]` | `0=PEAKING`, `1=LOW_SHELF`, `2=HIGH_SHELF` |

JNI 메서드 시그니처는 기존 호환을 위해 다음 형태를 유지합니다.

```kotlin
external fun updateConfig(
    preampDB: Float,
    enableLimiter: Boolean,
    filterTypes: IntArray,
    frequencies: FloatArray,
    gains: FloatArray,
    qFactors: FloatArray
)
```

배열은 최대 20 band로 cap 됩니다. 부족한 band는 flat/bypass로 취급되어 stale coefficient가 남지 않아야 합니다.

### Wrapper input structure target

문서화 대상 wrapper 구조의 목표는 UI slider model을 core `EqEngineConfig`로 바꾸는 **core adapter 입력**을 명확히 하는 것입니다.

```text
gainMin, gainMax, gains[20]
qMin,    qMax,    qFactors[20]
frequencies[20]
filterTypes[20]
```

정책:

- `gainMin/gainMax`는 `gains[20]`를 core config로 변환할 때 적용하는 clamp bound입니다.
- `qMin/qMax`는 `qFactors[20]`를 core config로 변환할 때 적용하는 clamp bound입니다.
- 이 min/max는 **UI 제약이 아니라 core adapter clamp 정책**입니다. UI는 별도 제품 계층이며 이 모듈의 구현 범위가 아닙니다.
- `frequencies[20]`와 `filterTypes[20]`는 Parametric EQ band 정의로 유지합니다.
- wrapper는 Linear-phase/Dynamic EQ용 확장 구조가 아니며, Parametric EQ 20-band 입력을 안정적으로 변환하기 위한 구조입니다.

---

## 🔌 Kotlin/JNI Binding

기본 Kotlin 클래스 경로는 C++의 `EQ_JNI_CLASS_PATH` 기본값과 맞춰야 합니다.

```kotlin
package com.example.audio

import java.nio.ByteBuffer

class AudioEngineJNI {
    init { System.loadLibrary("audioengine_jni") }

    external fun init(sampleRate: Int)
    external fun release()
    external fun updateConfig(
        preampDB: Float,
        enableLimiter: Boolean,
        filterTypes: IntArray,
        frequencies: FloatArray,
        gains: FloatArray,
        qFactors: FloatArray
    )
    external fun processDirectBuffer(buffer: ByteBuffer, numFrames: Int)
    external fun computeAutoPreampDB(): Float
}
```

중요 사항:

- 기본 `EQ_JNI_CLASS_PATH`는 `com/example/audio/AudioEngineJNI`입니다.
- `JNI_OnLoad`에서 `RegisterNatives`로 위 메서드들을 등록합니다.
- 다른 패키지명 또는 클래스명을 쓰려면 CMake/Gradle cppFlags에 `-DEQ_JNI_CLASS_PATH=\"com/your/pkg/YourClass\"`를 지정하고 Kotlin 클래스 경로도 동일하게 맞춥니다.
- 레거시 `Java_com_example_audio_AudioEngineJNI_*` C symbol fallback은 기본 class path 호환용입니다. 커스텀 class path에서는 `RegisterNatives` 기준으로 맞추는 것이 안전합니다.

---

## 🧩 Native C++ Usage

```cpp
#include "FineTuneEQEngine.h"

FineTuneEQEngine eq(48000.0);

EqEngineConfig cfg;
cfg.preampDB = 0.0f;
cfg.enableSoftLimiter = true;

EqBandConfig band;
band.type = EqFilterType::PEAKING;
band.frequency = 1000.0f;
band.gainDB = 3.0f;
band.qFactor = 0.707f;
cfg.bands.push_back(band);

eq.updateConfig(cfg);

float recommendedPreamp = eq.computeAutoPreampDB();
cfg.preampDB = recommendedPreamp;
eq.updateConfig(cfg);

float pcm[1024 * 2] = {}; // L,R,L,R...
eq.process(pcm, pcm, 1024);
```

---

## 📁 Project Structure

```text
AndroidEQModule/
├── README.md
├── DEVELOPER_GUIDE.md
└── src/main/cpp/
    ├── BiquadFilter.h / .cpp
    ├── FineTuneEQEngine.h / .cpp
    ├── AudioEngineJNI.cpp
    ├── RTSafetyUtils.h
    ├── DenormalGuard.h
    └── CMakeLists.txt
```

---

## ✅ Build & Verification

Android NDK build는 `src/main/cpp/CMakeLists.txt`를 기준으로 검증합니다.

```bash
NDK=/path/to/Android/sdk/ndk/27.0.12077973
cmake -S src/main/cpp -B build/android-arm64 \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-24 \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/android-arm64 -j
```

“모든 C++ 환경에서 동작”한다고 표현하려면 단순 개별 `clang++ -c` 확인이 아니라 **host CMake/standalone target**으로 실제 링크·실행 검증이 필요합니다. 현재 문서에서는 Android NDK target과 host standalone 검증 대상을 분리해 표현합니다.

---

## 📌 DSP Notes

| 항목 | 내용 |
|---|---|
| EQ 범위 | 20-band Parametric EQ |
| Filter types | `PEAKING`, `LOW_SHELF`, `HIGH_SHELF` |
| PCM format | Float32 stereo interleaved |
| Preamp | `computeAutoPreampDB()` 기반 누적 응답 peak 보정 |
| Limiter | optional soft limiter |
| Wrapper clamp | `gainMin/gainMax`, `qMin/qMax`는 core adapter 정책 |

---

## 📜 License

본 프로젝트의 라이선스는 별도 협의가 필요합니다.
