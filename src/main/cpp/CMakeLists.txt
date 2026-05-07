cmake_minimum_required(VERSION 3.22.1)

project("AndroidEQModule")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(DEFINED ANDROID_ABI)
    set(EQ_BUILD_JNI_DEFAULT ON)
else()
    set(EQ_BUILD_JNI_DEFAULT OFF)
endif()

option(BUILD_STANDALONE_TEST "Build the host/standalone regression harness" OFF)
option(BUILD_JNI "Build the Android JNI shared library" ${EQ_BUILD_JNI_DEFAULT})

add_library(
        eq_core
        STATIC
        BiquadFilter.cpp
        FineTuneEQEngine.cpp
)

set_target_properties(eq_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(
        eq_core
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_options(
        eq_core
        PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

if(DEFINED ANDROID_ABI AND ANDROID_ABI STREQUAL "armeabi-v7a")
    target_compile_options(eq_core PRIVATE -mfpu=neon)
endif()

if(BUILD_JNI)
    if(NOT DEFINED ANDROID_ABI)
        message(FATAL_ERROR "BUILD_JNI=ON requires an Android NDK CMake build. Use -DBUILD_JNI=OFF for host C++ builds.")
    endif()

    add_library(
            audioengine_jni
            SHARED
            AudioEngineJNI.cpp
    )

    target_link_libraries(
            audioengine_jni
            PRIVATE
            eq_core
            android
            log
    )
endif()

if(BUILD_STANDALONE_TEST)
    add_executable(
            standalone_test
            standalone_test.cpp
    )

    target_link_libraries(
            standalone_test
            PRIVATE
            eq_core
    )

    target_compile_options(
            standalone_test
            PRIVATE
            -Wall
            -Wextra
            -Wpedantic
    )
endif()
