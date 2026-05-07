# 🛠️ Android Custom 20-Band EQ 모듈: 개발자 및 포팅 가이드

본 가이드는 C++로 작성된 `AndroidEQModule`을 실제 안드로이드 스튜디오(Kotlin/Java) 프로젝트에 완벽하게 결합(Porting)하고, 오디오 파이프라인을 구축하기 위한 A to Z 상세 매뉴얼입니다.

---

## 📦 1. 안드로이드 스튜디오 프로젝트로 포팅 (Porting)

### 1.1. C++ 소스 파일 복사
현재 폴더에 있는 `src/main/cpp/` 내부의 모든 C++ 파일 및 헤더를 안드로이드 스튜디오 프로젝트의 `app/src/main/cpp/` 경로로 복사합니다.

### 1.2. `CMakeLists.txt` 설정 (모듈 연결)
안드로이드 앱의 `app/src/main/cpp/CMakeLists.txt` 파일을 열어 다음 코드를 추가합니다.

```cmake
# AndroidEQModule 소스 지정
add_library(
    audioengine_jni
    SHARED
    BiquadFilter.cpp
    FineTuneEQEngine.cpp
    AudioEngineJNI.cpp
)

# NDK 기본 라이브러리 링크 (log는 디버깅용)
target_link_libraries(
    audioengine_jni
    android
    log
)
```

### 1.3. `build.gradle` NDK 활성화
`app/build.gradle`의 `android.defaultConfig` 블록에 C++ 표준 및 NEON 가속을 명시합니다.

```groovy
android {
    defaultConfig {
        externalNativeBuild {
            cmake {
                cppFlags "-std=c++17 -mfpu=neon"
            }
        }
    }
    externalNativeBuild {
        cmake {
            path "src/main/cpp/CMakeLists.txt"
        }
    }
}
```

---

## 💻 2. Kotlin 연동 보일러플레이트 (Boilerplate)

JNI 코드를 호출하기 위해 Kotlin 측에 브릿지 클래스를 생성해야 합니다. 패키지명은 `com.example.audio`로 되어 있으며, 필요 시 JNI C++ 파일의 매크로 이름을 본인의 패키지명에 맞게 변경해야 합니다.

### 2.1. JNI Wrapper 클래스 작성 (`AudioEngineJNI.kt`)

> ⚠️ **클래스명은 반드시 `AudioEngineJNI`로 일치시켜야 합니다.** C++ JNI 심볼이 `Java_com_example_audio_AudioEngineJNI_*` 형태로 export되어 있어, 클래스명이 다르면 `UnsatisfiedLinkError`가 발생합니다. 다른 이름을 쓰려면 빌드 시 `-DEQ_JNI_CLASS_PATH='"com/your/pkg/YourClass"'`를 주거나 C 심볼을 모두 변경해야 합니다.

```kotlin
package com.example.audio

import java.nio.ByteBuffer

class AudioEngineJNI {
    // C++ 라이브러리 로드
    init {
        System.loadLibrary("audioengine_jni")
    }

    // C++ 내부 메모리 할당
    external fun init(sampleRate: Int)
    
    // 메모리 해제
    external fun release()

    // 20밴드 EQ 업데이트 (Preamp, Limiter 포함)
    external fun updateConfig(
        preampDB: Float,
        enableLimiter: Boolean,
        filterTypes: IntArray, // 0=PEAKING, 1=LOW_SHELF, 2=HIGH_SHELF
        frequencies: FloatArray,
        gains: FloatArray,
        qFactors: FloatArray
    )

    // 핵심: Direct ByteBuffer를 C++로 던져 In-place 처리
    external fun processDirectBuffer(buffer: ByteBuffer, numFrames: Int)
}
```

### 2.2. 오디오 스트리밍 루프 및 Direct ByteBuffer 할당

PCM을 디코더에서 꺼내어 `AudioTrack`으로 보낼 때 징검다리 역할을 하는 핵심 루프입니다.

```kotlin
import android.media.AudioFormat
import android.media.AudioTrack
import java.nio.ByteBuffer
import java.nio.ByteOrder

fun startAudioProcessingLoop() {
    val sampleRate = 48000
    val channels = 2 // 스테레오
    
    // 1. C++ 엔진 초기화
    val engine = AudioEngineJNI()
    engine.init(sampleRate)

    // 2. 초기 20밴드 설정 주입 (플랫 + 리미터 On)
    val dummyArray = FloatArray(20) { 0f }
    val typesArray = IntArray(20) { 0 }
    engine.updateConfig(0f, true, typesArray, dummyArray, dummyArray, dummyArray)

    // 3. AudioTrack 설정 (반드시 ENCODING_PCM_FLOAT 사용!)
    val audioTrack = AudioTrack.Builder()
        .setAudioFormat(AudioFormat.Builder()
            .setEncoding(AudioFormat.ENCODING_PCM_FLOAT)
            .setSampleRate(sampleRate)
            .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
            .build())
        .build()

    // 4. Direct ByteBuffer 생성 (JNI Zero-copy의 핵심)
    // 1프레임 = 2채널(L/R) * 4바이트(Float) = 8바이트. (예: 1024 프레임 버퍼)
    val numFrames = 1024
    val byteBufferSize = numFrames * 2 * 4 
    val directBuffer = ByteBuffer.allocateDirect(byteBufferSize)
        .order(ByteOrder.nativeOrder())

    // 5. 오디오 스레드 권한 격상 (필수)
    android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_URGENT_AUDIO)

    audioTrack.play()

    // 6. 실시간 처리 루프
    while (isPlaying) {
        // [A] 디코더에서 PCM 데이터(Float)를 가져와 directBuffer에 기록 (구현 생략)
        fillBufferFromDecoder(directBuffer)

        // [B] C++ EQ 엔진 통과 (In-place 연산: directBuffer 내부의 값이 EQ가 먹힌 소리로 변함)
        engine.processDirectBuffer(directBuffer, numFrames)

        // [C] 처리된 버퍼를 AudioTrack으로 출력
        directBuffer.position(0)
        audioTrack.write(directBuffer, byteBufferSize, AudioTrack.WRITE_BLOCKING)
    }

    engine.release()
}
```

---

## 🚦 3. 개발자 핵심 팁 및 트러블슈팅 (Troubleshooting)

### Q1. 소리가 심하게 찢어집니다 (Clipping/Distortion)
가장 흔하게 발생하는 문제입니다.
1. `AudioTrack`을 **`ENCODING_PCM_FLOAT`**로 설정하셨는지 확인하세요. 16-bit Short로 보내면 소리가 깨집니다.
2. 디코더에서 나온 데이터가 16-bit라면 Kotlin 루프 내에서 `-32768 ~ 32767` 값을 `-1.0f ~ 1.0f`로 나누어 Float 변환한 뒤 `directBuffer`에 넣어야 합니다.
3. **Preamp 누락 여부:** +5dB로 부스트된 슬라이더가 있다면, Kotlin에서 `engine.updateConfig`를 호출할 때 반드시 `preampDB` 인자로 `-5.0f`를 넘겨주었는지 확인하세요. (자동 감쇠 누락 시 리미터가 과도하게 걸립니다)

### Q2. EQ 파라미터(슬라이더)를 바꿀 때마다 틱(Click)/팝(Pop) 노이즈가 납니다
C++ 엔진 객체(`new FineTuneEQEngine()`)를 매번 새로 생성하시면 안 됩니다!
오디오가 재생 중일 때 슬라이더를 드래그하면, 백그라운드에서 오디오가 도는 상태 그대로 `engine.updateConfig(...)` 함수만 런타임에 계속 쏴주시면 됩니다. 내부의 Stateful Biquad 필터가 자연스럽게 계수를 전환하며 팝 노이즈를 억제합니다.

### Q3. UI 슬라이더를 올렸는데 반응이 느립니다 (Latency)
버퍼 사이즈(`numFrames`)를 너무 크게 잡아서 그렇습니다. JNI 호출을 아끼려고 8192 프레임(약 170ms) 씩 던지면 소리도 170ms 뒤에 변합니다.
모바일 최적값은 **480 ~ 1024 프레임(10ms ~ 20ms)** 단위입니다.

### Q4. 패키지명을 바꾸고 싶어요
`AudioEngineJNI.cpp` 내부의 함수명을 유심히 보십시오.
`Java_com_example_audio_AudioEngineJNI_init` 은 `com.example.audio` 패키지의 `AudioEngineJNI` 클래스에 바인딩된다는 JNI 규격입니다.
만약 본인의 프로젝트 패키지가 `com.mycompany.music`이고 코틀린 클래스가 `EqCore` 라면, C++ 함수명들을 모두 `Java_com_mycompany_music_EqCore_init`으로 변경한 후 빌드하셔야 `UnsatisfiedLinkError`가 나지 않습니다.
