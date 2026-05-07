cmake_minimum_required(VERSION 3.22.1)

project("AndroidEQModule")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(${ANDROID_ABI} STREQUAL "armeabi-v7a")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mfpu=neon")
endif()

add_library(
        audioengine_jni
        SHARED
        BiquadFilter.cpp
        FineTuneEQEngine.cpp
        AudioEngineJNI.cpp
)

target_link_libraries(
        audioengine_jni
        android
        log
)
