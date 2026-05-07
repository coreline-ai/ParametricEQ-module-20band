# 🛠️ AndroidEQModule Developer Guide

본 문서는 `AndroidEQModule`의 현재 JNI 입력 계약과 wrapper 구조 목표를 정리한 개발자 가이드입니다. 범위는 **20-band Parametric EQ core/JNI 연동**에 한정합니다.

---

## 1. Scope Boundary

포함 범위:

- 20-band Parametric EQ 설정 전달
- C++ core `EqEngineConfig` 변환
- JNI `AudioEngineJNI` binding
- Direct `ByteBuffer` 기반 Float32 stereo PCM in-place 처리
- `computeAutoPreampDB()` 기반 권장 preamp 계산

범위 밖:

- Linear-phase EQ 구현
- Dynamic EQ 구현
- Oboe player 구현
- Android UI/slider 화면 구현

Linear-phase/Dynamic EQ는 Parametric EQ와의 비교 또는 제외 범위 설명에만 사용할 수 있습니다. 이 문서에서는 추가 구현 계획으로 다루지 않습니다.

---

## 2. Current Kotlin/JNI Contract

현재 기본 입력은 다음 4개 20-band 배열입니다.

```text
gains[20]
qFactors[20]
frequencies[20]
filterTypes[20]
```

Kotlin wrapper는 기본 class path 기준으로 다음처럼 둡니다.

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
        filterTypes: IntArray,    // 0=PEAKING, 1=LOW_SHELF, 2=HIGH_SHELF
        frequencies: FloatArray,  // Hz
        gains: FloatArray,        // dB
        qFactors: FloatArray      // PEAKING=Q, LOW/HIGH_SHELF=RBJ shelf slope S
    )

    external fun processDirectBuffer(buffer: ByteBuffer, numFrames: Int)
    external fun computeAutoPreampDB(): Float
}
```

주의:

- JNI signature는 `(FZ[I[F[F[F)V`입니다.
- 배열 길이는 최대 20개로 취급합니다.
- 20개보다 적은 입력은 나머지 band를 flat/bypass로 다루는 방향이어야 합니다.
- `filterTypes` 값은 `0=PEAKING`, `1=LOW_SHELF`, `2=HIGH_SHELF` Parametric EQ 범위 안에서만 사용합니다.
- `qFactors`의 의미는 filter type에 따라 다릅니다. `PEAKING`은 Q factor이고, `LOW_SHELF`/`HIGH_SHELF`는 RBJ Cookbook의 shelf slope `S`로 해석합니다.

---

## 3. Wrapper Input Structure Target

새 wrapper 입력 구조의 목표는 Kotlin/UI 계층에서 온 고정 배열 값을 core config로 변환하는 adapter contract를 명확히 하는 것입니다.

```text
gainMin: Float
gainMax: Float
gains[20]: FloatArray

qMin: Float
qMax: Float
qFactors[20]: FloatArray

frequencies[20]: FloatArray
filterTypes[20]: IntArray 또는 EqFilterType[20]
```

정책:

- `gainMin/gainMax`는 `gains[20]`를 clamp하는 core adapter bound입니다.
- `qMin/qMax`는 `qFactors[20]`를 clamp하는 core adapter bound입니다.
- min/max는 UI slider 제한이 아닙니다. UI가 어떤 범위를 표시하든 core adapter는 자체 clamp 정책을 가져야 합니다.
- wrapper는 기존 `EqEngineConfig`/`EqBandConfig`로 변환되는 입력 계층입니다.
- wrapper 구조는 Parametric EQ 전용입니다. Linear-phase/Dynamic EQ 확장을 전제로 필드를 추가하지 않습니다.

예상 변환 흐름:

```text
Wrapper fixed arrays
  → clamp gains by gainMin/gainMax
  → clamp qFactors by qMin/qMax
  → pair with frequencies/filterTypes
  → EqBandConfig[0..20]
  → EqEngineConfig
  → FineTuneEQEngine.updateConfig(...)
```

---

## 4. Preamp Policy

Preamp는 “가장 큰 band gain이 +5 dB이므로 -5 dB를 넣는다” 같은 단순 규칙으로 설명하지 않습니다.

권장 정책:

1. `updateConfig(...)`로 band 설정을 반영합니다.
2. `computeAutoPreampDB()`를 호출합니다.
3. 반환된 값을 `preampDB`로 다시 적용합니다.

`computeAutoPreampDB()`는 개별 band gain의 최대값이 아니라, 20-band cascade의 **누적 주파수 응답 peak**를 기준으로 권장 감쇠량을 계산합니다. 여러 band가 겹쳐 실제 peak가 커지는 경우를 반영할 수 있으며, 순수 감쇠 preset에서는 0 dB를 반환할 수 있습니다.

---

## 5. JNI Class Path and RegisterNatives

C++ bridge는 `JNI_OnLoad`에서 `RegisterNatives`를 사용합니다.

- 기본 class path: `com/example/audio/AudioEngineJNI`
- Kotlin 기본 클래스: `com.example.audio.AudioEngineJNI`
- build-time override: `-DEQ_JNI_CLASS_PATH=\"com/your/pkg/YourClass\"`

권장 방식:

```groovy
android {
    defaultConfig {
        externalNativeBuild {
            cmake {
                cppFlags '-std=c++17 -DEQ_JNI_CLASS_PATH="com/your/pkg/YourClass"'
            }
        }
    }
}
```

충돌 방지 원칙:

- Kotlin 클래스 경로와 `EQ_JNI_CLASS_PATH`를 동일하게 유지합니다.
- 메서드 이름/signature는 `RegisterNatives` 테이블과 맞춥니다.
- 기본으로 export된 `Java_com_example_audio_AudioEngineJNI_*` C symbol은 fallback 호환용으로 이해합니다.
- 커스텀 패키지/클래스에서는 C symbol 이름을 직접 의존하지 말고 `RegisterNatives` + `EQ_JNI_CLASS_PATH` 기준으로 맞춥니다.

---

## 6. Build and Verification Notes

Android NDK build 예시:

```bash
NDK=/path/to/Android/sdk/ndk/27.0.12077973
cmake -S src/main/cpp -B build/android-arm64 \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-24 \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/android-arm64 -j
```

Host C++ 검증은 “모든 C++ 환경”이라는 표현을 쓰기 전에 별도 standalone target으로 확인해야 합니다.

- 단순 `clang++ -c`는 컴파일 단위 검증일 뿐입니다.
- host CMake/standalone executable 또는 test target을 구성해 link와 runtime smoke test까지 확인해야 합니다.
- JNI/Android 의존 파일은 host target에서 제외하거나 stub 처리해야 합니다.

Host standalone 검증 예시:

```bash
cmake -S src/main/cpp -B build/host \
  -DBUILD_STANDALONE_TEST=ON \
  -DBUILD_JNI=OFF \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/host -j
./build/host/standalone_test
```

---

## 7. Troubleshooting

| 증상 | 확인 사항 |
|---|---|
| `UnsatisfiedLinkError` | Kotlin class path와 `EQ_JNI_CLASS_PATH` 일치 여부, `System.loadLibrary("audioengine_jni")` 호출 여부 |
| EQ 적용 후 clipping | `computeAutoPreampDB()` 기반 preamp를 적용했는지 확인 |
| 설정 변경 시 click/pop | 엔진 재생성 대신 기존 instance에 `updateConfig(...)`만 호출하는지 확인 |
| 배열 입력 오류 | 4개 배열이 모두 준비되었는지, 20-band cap 정책과 null/길이 검증을 통과하는지 확인 |
| PCM 처리 실패 | `ByteBuffer.allocateDirect(...)`, native byte order, `numFrames * 2 * sizeof(float)` capacity 확인 |

---

## 8. Documentation Rules for Parallel Work

- 이 문서는 Parametric EQ wrapper/core adapter 범위만 기술합니다.
- Linear-phase EQ, Dynamic EQ, Oboe player, UI 구현을 roadmap 또는 추가 계획으로 넣지 않습니다.
- 구현 파일(`src/main/cpp/*`)과 개발 계획 파일(`dev-plan/*`)은 이 문서 동기화 작업의 수정 대상이 아닙니다.
