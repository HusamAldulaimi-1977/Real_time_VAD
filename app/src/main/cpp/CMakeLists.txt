cmake_minimum_required(VERSION 3.22.1)
project("real_time_vad")

add_library(vadnative SHARED
        native-lib.cpp
        SuperpoweredIO.cpp
        FeatureExtractor.cpp
        superpowered/OpenSource/SuperpoweredAndroidAudioIO.cpp
        AudioPipeline.cpp
        FIRFilter.c

)

# headers
target_include_directories(vadnative PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/superpowered
        ${CMAKE_CURRENT_LIST_DIR}/superpowered/OpenSource
)

# Android log
find_library(log-lib log)

# ---- Pick the correct Superpowered static lib based on ABI ----
set(SUPERPOWERED_LIB "")
if(ANDROID_ABI STREQUAL "armeabi-v7a")
    set(SUPERPOWERED_LIB ${CMAKE_CURRENT_LIST_DIR}/superpowered/libSuperpoweredAndroidarmeabi-v7a.a)
elseif(ANDROID_ABI STREQUAL "arm64-v8a")
    set(SUPERPOWERED_LIB ${CMAKE_CURRENT_LIST_DIR}/superpowered/libSuperpoweredAndroidarm64-v8a.a)
elseif(ANDROID_ABI STREQUAL "x86")
    set(SUPERPOWERED_LIB ${CMAKE_CURRENT_LIST_DIR}/superpowered/libSuperpoweredAndroidX86.a)
elseif(ANDROID_ABI STREQUAL "x86_64")
    set(SUPERPOWERED_LIB ${CMAKE_CURRENT_LIST_DIR}/superpowered/libSuperpoweredAndroidX86_64.a)
else()
    message(FATAL_ERROR "Unsupported ANDROID_ABI: ${ANDROID_ABI}")
endif()

add_library(superpowered STATIC IMPORTED)
set_target_properties(superpowered PROPERTIES
        IMPORTED_LOCATION ${SUPERPOWERED_LIB}
)

target_link_libraries(vadnative
        superpowered
        OpenSLES
        android
        ${log-lib}
)
