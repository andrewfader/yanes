cmake_minimum_required(VERSION 3.20)
project(YANES VERSION 0.1.0 LANGUAGES CXX)
find_package(X11 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(XFT REQUIRED IMPORTED_TARGET xft)

include(FetchContent)
if(DEFINED CLAP_ROOT)
  set(clap_SOURCE_DIR "${CLAP_ROOT}")
else()
  FetchContent_Declare(clap
    GIT_REPOSITORY https://github.com/free-audio/clap.git
    GIT_TAG 1.2.7
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(clap)
endif()

FetchContent_Declare(ymfm
  GIT_REPOSITORY https://github.com/aaronsgiles/ymfm.git
  GIT_TAG 81aec25ccbb98f4873a255f7551ac4dadac59b4a
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(ymfm)
add_library(yanes_ymfm STATIC
  ${ymfm_SOURCE_DIR}/src/ymfm_adpcm.cpp
  ${ymfm_SOURCE_DIR}/src/ymfm_opl.cpp
  ${ymfm_SOURCE_DIR}/src/ymfm_opm.cpp
  ${ymfm_SOURCE_DIR}/src/ymfm_opn.cpp
  ${ymfm_SOURCE_DIR}/src/ymfm_pcm.cpp
  ${ymfm_SOURCE_DIR}/src/ymfm_ssg.cpp)
target_include_directories(yanes_ymfm SYSTEM PUBLIC ${ymfm_SOURCE_DIR}/src)
target_compile_features(yanes_ymfm PUBLIC cxx_std_14)
set_target_properties(yanes_ymfm PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(yanes_clap MODULE src/yanes.cpp src/hardware_fm.cpp)
target_compile_features(yanes_clap PRIVATE cxx_std_20)
target_include_directories(yanes_clap PRIVATE "${clap_SOURCE_DIR}/include")
target_link_libraries(yanes_clap PRIVATE X11::X11 PkgConfig::XFT)
target_link_libraries(yanes_clap PRIVATE yanes_ymfm)
target_compile_options(yanes_clap PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
set_target_properties(yanes_clap PROPERTIES
  PREFIX ""
  OUTPUT_NAME "YANES"
  SUFFIX ".clap"
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES)

install(TARGETS yanes_clap LIBRARY DESTINATION lib/clap)

add_executable(yanes-dpcm tools/yanes_dpcm.cpp)
target_compile_features(yanes-dpcm PRIVATE cxx_std_20)
target_compile_options(yanes-dpcm PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS yanes-dpcm RUNTIME DESTINATION bin)

add_executable(yanes-register-render tools/yanes_register_render.cpp)
target_compile_features(yanes-register-render PRIVATE cxx_std_20)
target_link_libraries(yanes-register-render PRIVATE yanes_ymfm)
install(TARGETS yanes-register-render RUNTIME DESTINATION bin)

add_executable(yanes-audio-compare tools/yanes_audio_compare.cpp)
target_compile_features(yanes-audio-compare PRIVATE cxx_std_20)
install(TARGETS yanes-audio-compare RUNTIME DESTINATION bin)

add_executable(yanes-parity-compare tools/yanes_parity_compare.cpp)
target_compile_features(yanes-parity-compare PRIVATE cxx_std_20)
target_compile_options(yanes-parity-compare PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS yanes-parity-compare RUNTIME DESTINATION bin)

add_executable(yanes-vgm-extract tools/yanes_vgm_extract.cpp)
target_compile_features(yanes-vgm-extract PRIVATE cxx_std_20)
install(TARGETS yanes-vgm-extract RUNTIME DESTINATION bin)

add_executable(vgm_tests tests/vgm_tests.cpp)
target_compile_features(vgm_tests PRIVATE cxx_std_20)
target_compile_options(vgm_tests PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
install(PROGRAMS tools/compare_furnace.sh DESTINATION bin RENAME yanes-compare-furnace)

enable_testing()
add_executable(yanes-fixture-render tests/furnace_render.cpp)
target_compile_features(yanes-fixture-render PRIVATE cxx_std_20)
target_compile_options(yanes-fixture-render PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
target_include_directories(yanes-fixture-render PRIVATE "${clap_SOURCE_DIR}/include")
target_link_libraries(yanes-fixture-render PRIVATE dl)
add_test(NAME vgm_tests COMMAND vgm_tests $<TARGET_FILE:yanes-vgm-extract>)
add_executable(dsp_tests tests/dsp_tests.cpp)
target_compile_features(dsp_tests PRIVATE cxx_std_20)
target_compile_options(dsp_tests PRIVATE -UNDEBUG)
add_test(NAME dsp_tests COMMAND dsp_tests)

add_executable(ui_tests tests/ui_tests.cpp)
target_compile_features(ui_tests PRIVATE cxx_std_20)
target_compile_options(ui_tests PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
target_include_directories(ui_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
add_test(NAME ui_tests COMMAND ui_tests)

add_executable(plugin_smoke tests/plugin_smoke.cpp)
target_compile_features(plugin_smoke PRIVATE cxx_std_20)
target_compile_options(plugin_smoke PRIVATE -UNDEBUG)
target_include_directories(plugin_smoke PRIVATE "${clap_SOURCE_DIR}/include")
target_link_libraries(plugin_smoke PRIVATE dl)
add_test(NAME plugin_smoke COMMAND plugin_smoke $<TARGET_FILE:yanes_clap>)

# Behavioural suites that drive the built plug-in through the public CLAP interfaces.
foreach(suite note_tests state_tests bank_tests)
  add_executable(${suite} tests/${suite}.cpp)
  target_compile_features(${suite} PRIVATE cxx_std_20)
  target_compile_options(${suite} PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
  target_include_directories(${suite} PRIVATE "${clap_SOURCE_DIR}/include")
  target_link_libraries(${suite} PRIVATE dl)
  add_test(NAME ${suite} COMMAND ${suite} $<TARGET_FILE:yanes_clap>)
endforeach()
set_tests_properties(bank_tests PROPERTIES
  ENVIRONMENT "YANES_DPCM_TOOL=$<TARGET_FILE:yanes-dpcm>")
add_test(NAME ymfm_register_render COMMAND yanes-register-render ym2612
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/ym2612_tone.reg
  ${CMAKE_CURRENT_BINARY_DIR}/ym2612_tone.wav)
set_tests_properties(ymfm_register_render PROPERTIES FIXTURES_SETUP ymfm_tone)
add_test(NAME audio_compare COMMAND yanes-audio-compare
  ${CMAKE_CURRENT_BINARY_DIR}/ym2612_tone.wav ${CMAKE_CURRENT_BINARY_DIR}/ym2612_tone.wav 0.9999)
set_tests_properties(audio_compare PROPERTIES FIXTURES_REQUIRED ymfm_tone)
find_program(REAPER_EXECUTABLE reaper)
if(REAPER_EXECUTABLE)
  add_test(NAME reaper_clap_integration COMMAND
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_reaper_integration.sh)
  set_tests_properties(reaper_clap_integration PROPERTIES TIMEOUT 120 ENVIRONMENT
    "REAPER=${REAPER_EXECUTABLE};YANES_CLAP=$<TARGET_FILE:yanes_clap>;YANES_AUDIO_COMPARE=$<TARGET_FILE:yanes-audio-compare>")
endif()
add_test(NAME ymfm_opm_register_render COMMAND yanes-register-render ym2151
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/ym2151_tone.reg
  ${CMAKE_CURRENT_BINARY_DIR}/ym2151_tone.wav)
foreach(chip ym2203 ym2608 ym3812 ymf262)
  add_test(NAME ${chip}_register_render COMMAND yanes-register-render ${chip}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/ym2612_tone.reg
    ${CMAKE_CURRENT_BINARY_DIR}/${chip}_tone.wav)
endforeach()

# Optional external-oracle suite. Supply a directory containing ym2203.fur,
# ym2608.fur, ym2612.fur, ym2151.fur, ym3812.fur and ymf262.fur to enable it.
set(YANES_FURNACE_FIXTURE_DIR "" CACHE PATH "Directory containing per-chip Furnace parity fixtures")
set(YANES_FURNACE_MIN_CORRELATION "0.4" CACHE STRING "Minimum Furnace envelope correlation")
if(YANES_FURNACE_FIXTURE_DIR)
  find_program(FURNACE_EXECUTABLE furnace REQUIRED)
  foreach(chip ym2203 ym2608 ym2612 ym2151 ym3812 ymf262)
    set(module "${YANES_FURNACE_FIXTURE_DIR}/${chip}.fur")
    if(NOT EXISTS "${module}")
      message(FATAL_ERROR "Missing Furnace parity fixture: ${module}")
    endif()
    add_test(NAME furnace_${chip}_parity COMMAND
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/compare_furnace.sh ${chip} ${module}
      ${YANES_FURNACE_MIN_CORRELATION})
    set_tests_properties(furnace_${chip}_parity PROPERTIES ENVIRONMENT
      "PATH=$ENV{PATH};YANES_REGISTER_RENDER=$<TARGET_FILE:yanes-register-render>;YANES_VGM_EXTRACT=$<TARGET_FILE:yanes-vgm-extract>;YANES_AUDIO_COMPARE=$<TARGET_FILE:yanes-audio-compare>")
  endforeach()
endif()

# Full chip-audio oracle suite. Point this at the Furnace version to certify;
# the repository fixtures and the 0.80 composite thresholds remain fixed.
set(YANES_FURNACE_EXECUTABLE "" CACHE FILEPATH "Furnace executable for the 21-chip audio parity suite")
if(YANES_FURNACE_EXECUTABLE)
  foreach(fixture ay-tone fds gameboy-noise gameboy-pulse gameboy-wave n163
      nes-noise nes-pulse nes-triangle pce-wave pokey-tone saa1099 scc sid6581
      sid8580 sms-noise sms-tone tia vrc6-pulse vrc6-saw vrc7)
    add_test(NAME furnace_audio_${fixture} COMMAND
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/compare_furnace_audio.sh ${fixture}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/furnace/${fixture}.fur 0.8)
    set_tests_properties(furnace_audio_${fixture} PROPERTIES ENVIRONMENT
      "FURNACE=${YANES_FURNACE_EXECUTABLE};YANES_FIXTURE_RENDER=$<TARGET_FILE:yanes-fixture-render>;YANES_CLAP=$<TARGET_FILE:yanes_clap>;YANES_PARITY_COMPARE=$<TARGET_FILE:yanes-parity-compare>")
  endforeach()
  foreach(fixture ay-tone fds gameboy-noise gameboy-pulse gameboy-wave n163
      nes-noise nes-pulse nes-triangle pce-wave pokey-tone saa1099 scc sid6581
      sid8580 sms-noise sms-tone tia vrc6-pulse vrc6-saw vrc7)
    foreach(octave low high)
      add_test(NAME furnace_audio_${fixture}_${octave} COMMAND
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/compare_furnace_audio.sh ${fixture}-${octave}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/furnace_holdout/${fixture}-${octave}.fur 0.8)
      set_tests_properties(furnace_audio_${fixture}_${octave} PROPERTIES ENVIRONMENT
        "FURNACE=${YANES_FURNACE_EXECUTABLE};YANES_FIXTURE_RENDER=$<TARGET_FILE:yanes-fixture-render>;YANES_CLAP=$<TARGET_FILE:yanes_clap>;YANES_PARITY_COMPARE=$<TARGET_FILE:yanes-parity-compare>")
    endforeach()
  endforeach()
endif()
