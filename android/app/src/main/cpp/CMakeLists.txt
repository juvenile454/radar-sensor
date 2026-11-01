# A5: DSP-Header der Firmware in der App, zum Nachrechnen von iq3-raw-Aufnahmen (gleiche Quelle wie am PC).
# V5.2: zweite Bibliothek radardsp_live für die Live-DSP am Handy aus @RAW, mit den schwereren Varianten
# (A121_DSP_HEAVY = Viterbi + Abzug der Atem-Oberwellen). Getrennte Bibliotheken, weil die Schalter das Layout der
# DSP-Klasse ändern.
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
        ${REPO_ROOT}/host/dsp_replay          # dsp_replay.h, dsp_live.h + Arduino.h-Ersatz
        ${REPO_ROOT}/firmware/esp32s3/src)    # a121_esp_dsp.h
    set_target_properties(${lib} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    # Auch im Debug-Build optimiert (sonst langsam) und ohne zusammengefasste Multiply-Add-Befehle: Ergebnis soll
    # bitgleich zur PC-Fassung (g++ -O2 -ffp-contract=off) sein.
    target_compile_options(${lib} PRIVATE -O2 -ffp-contract=off -fno-fast-math -Wall)
    target_link_libraries(${lib} PRIVATE m log)
endforeach()
