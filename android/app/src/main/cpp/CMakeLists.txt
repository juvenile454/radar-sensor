# A5: firmware DSP header in the app, to replay iq3-raw recordings (same source as on the PC).
# V5.2: second library radardsp_live for the live DSP on the phone from @RAW, with the heavier variants
# (A121_DSP_HEAVY = Viterbi + subtraction of breathing harmonics). Separate libraries because the compile switches
# change the layout of the DSP class.
cmake_minimum_required(VERSION 3.22.1)
project(radardsp CXX)

set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)

foreach(lib radardsp radardsp_live)
    if(lib STREQUAL "radardsp")
        add_library(${lib} SHARED radar_dsp_jni.cpp)
    else()
        add_library(${lib} SHARED radar_dsp_live_jni.cpp)
        target_compile_definitions(${lib} PRIVATE A121_DSP_HEAVY)
    endif()
    target_include_directories(${lib} PRIVATE
        ${REPO_ROOT}/host/dsp_replay          # dsp_replay.h, dsp_live.h + Arduino.h replacement
        ${REPO_ROOT}/firmware/esp32s3/src)    # a121_esp_dsp.h
    set_target_properties(${lib} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    # Optimized in the debug build too (otherwise slow) and without fused multiply-add instructions: the result should
    # be bit-identical to the PC version (g++ -O2 -ffp-contract=off).
    target_compile_options(${lib} PRIVATE -O2 -ffp-contract=off -fno-fast-math -Wall)
    target_link_libraries(${lib} PRIVATE m log)
endforeach()
