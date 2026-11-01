# A5: DSP-Header der Firmware in der App, nur zum Nachrechnen von iq3-raw-Aufnahmen (gleiche Quelle wie am PC).
cmake_minimum_required(VERSION 3.22.1)
project(radardsp CXX)

set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)

add_library(radardsp SHARED radar_dsp_jni.cpp)
target_include_directories(radardsp PRIVATE
    ${REPO_ROOT}/host/dsp_replay          # dsp_replay.h + Arduino.h-Ersatz
    ${REPO_ROOT}/firmware/esp32s3/src)    # a121_esp_dsp.h
set_target_properties(radardsp PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
# Auch im Debug-Build optimiert (sonst langsam) und ohne zusammengefasste Multiply-Add-Befehle: Ergebnis soll
# bitgleich zur PC-Fassung (g++ -O2 -ffp-contract=off) sein.
target_compile_options(radardsp PRIVATE -O2 -ffp-contract=off -fno-fast-math -Wall)
target_link_libraries(radardsp PRIVATE m log)
