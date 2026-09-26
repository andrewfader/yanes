cmake_minimum_required(VERSION 3.20)
project(YANES VERSION 0.3.0 LANGUAGES CXX)

# Keep windows.h from defining the min/max macros no matter which header pulls it in.
add_compile_definitions($<$<CXX_COMPILER_ID:MSVC>:NOMINMAX>)

function(yanes_warnings target)
  target_compile_options(${target} PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic>)
endfunction()

function(yanes_enable_assertions target)
  target_compile_options(${target} PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)
endfunction()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(X11 REQUIRED)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(XFT REQUIRED IMPORTED_TARGET xft)
endif()

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
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(yanes_clap PRIVATE X11::X11 X11::Xrender PkgConfig::XFT)
endif()
target_link_libraries(yanes_clap PRIVATE yanes_ymfm)
if(WIN32)
  target_link_libraries(yanes_clap PRIVATE user32 gdi32 comdlg32)
endif()
if(APPLE)
  enable_language(OBJCXX)
  set_source_files_properties(src/yanes.cpp PROPERTIES LANGUAGE OBJCXX)
  target_link_libraries(yanes_clap PRIVATE "-framework Cocoa")
endif()
target_compile_options(yanes_clap PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/W4>
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic;-Wconversion>)
set_target_properties(yanes_clap PROPERTIES
  PREFIX ""
  OUTPUT_NAME "YANES"
  SUFFIX ".clap"
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES)
if(APPLE)
  set_target_properties(yanes_clap PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION clap
    SUFFIX "")
endif()

install(TARGETS yanes_clap
  BUNDLE DESTINATION clap
  LIBRARY DESTINATION clap
  RUNTIME DESTINATION clap)

add_executable(yanes-dpcm tools/yanes_dpcm.cpp)
target_compile_features(yanes-dpcm PRIVATE cxx_std_20)
yanes_warnings(yanes-dpcm)
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
yanes_warnings(yanes-parity-compare)
install(TARGETS yanes-parity-compare RUNTIME DESTINATION bin)

add_executable(yanes-vgm-extract tools/yanes_vgm_extract.cpp)
target_compile_features(yanes-vgm-extract PRIVATE cxx_std_20)
install(TARGETS yanes-vgm-extract RUNTIME DESTINATION bin)

add_executable(yanes-nes-rom-emu tools/yanes_nes_rom_emu.cpp)
target_compile_features(yanes-nes-rom-emu PRIVATE cxx_std_20)
target_include_directories(yanes-nes-rom-emu PRIVATE "${clap_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/tests")
target_link_libraries(yanes-nes-rom-emu PRIVATE ${CMAKE_DL_LIBS})
yanes_warnings(yanes-nes-rom-emu)
yanes_enable_assertions(yanes-nes-rom-emu)

add_executable(yanes-gb-replay tools/yanes_gb_replay.cpp)
target_compile_features(yanes-gb-replay PRIVATE cxx_std_20)
yanes_warnings(yanes-gb-replay)
install(TARGETS yanes-gb-replay RUNTIME DESTINATION bin)

add_executable(yanes-pce-replay tools/yanes_pce_replay.cpp)
target_compile_features(yanes-pce-replay PRIVATE cxx_std_20)
yanes_warnings(yanes-pce-replay)
install(TARGETS yanes-pce-replay RUNTIME DESTINATION bin)

add_executable(yanes-nes-replay tools/yanes_nes_replay.cpp)
target_compile_features(yanes-nes-replay PRIVATE cxx_std_20)
yanes_warnings(yanes-nes-replay)
install(TARGETS yanes-nes-replay RUNTIME DESTINATION bin)

# The capture sides are intentionally not built here: they link against patched
# external SameBoy and Nestopia checkouts. tools/build_gb_oracle.sh and
# tools/build_nes_oracle.sh own those reproducible builds, while these targets
# keep the independent YANES replays in normal CI.

add_executable(vgm_tests tests/vgm_tests.cpp)
target_compile_features(vgm_tests PRIVATE cxx_std_20)
yanes_warnings(vgm_tests)
yanes_enable_assertions(vgm_tests)
install(PROGRAMS tools/compare_furnace.sh DESTINATION bin RENAME yanes-compare-furnace)

enable_testing()
add_test(NAME parity_compare_fairness COMMAND yanes-parity-compare --self-test)

# Optional real-ROM hardware-oracle gates. ROM images and patched emulator cores
# remain external; setting a ROM path makes the corresponding lane visible to
# CTest without bundling copyrighted data into this project.
set(YANES_GB_ROM "" CACHE FILEPATH "Game Boy ROM for the SameBoy parity gate")
set(YANES_PCE_ROM "" CACHE FILEPATH "PC Engine ROM for the Beetle PCE parity gate")
set(YANES_NES_ROM "" CACHE FILEPATH "NES ROM for the Nestopia parity gate")
if(YANES_GB_ROM)
  add_test(NAME gb_rom_parity COMMAND
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_gb_rom_parity.sh ${YANES_GB_ROM})
  set_tests_properties(gb_rom_parity PROPERTIES TIMEOUT 300)
endif()
if(YANES_PCE_ROM)
  add_test(NAME pce_rom_parity COMMAND
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_pce_rom_parity.sh ${YANES_PCE_ROM})
  set_tests_properties(pce_rom_parity PROPERTIES TIMEOUT 120)
endif()
if(YANES_NES_ROM)
  add_test(NAME nes_rom_parity COMMAND
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_nes_rom_parity.sh ${YANES_NES_ROM})
  set_tests_properties(nes_rom_parity PROPERTIES TIMEOUT 120)
endif()
add_executable(yanes-benchmark tools/yanes_benchmark.cpp)
target_compile_features(yanes-benchmark PRIVATE cxx_std_20)
target_include_directories(yanes-benchmark PRIVATE "${clap_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/tests")
target_link_libraries(yanes-benchmark PRIVATE ${CMAKE_DL_LIBS})
yanes_warnings(yanes-benchmark)
yanes_enable_assertions(yanes-benchmark)

add_executable(yanes-fixture-render tests/furnace_render.cpp)
target_compile_features(yanes-fixture-render PRIVATE cxx_std_20)
yanes_warnings(yanes-fixture-render)
yanes_enable_assertions(yanes-fixture-render)
target_include_directories(yanes-fixture-render PRIVATE "${clap_SOURCE_DIR}/include")
target_link_libraries(yanes-fixture-render PRIVATE ${CMAKE_DL_LIBS})
add_test(NAME vgm_tests COMMAND vgm_tests $<TARGET_FILE:yanes-vgm-extract>)
add_executable(audio_compare_tests tests/audio_compare_tests.cpp)
target_compile_features(audio_compare_tests PRIVATE cxx_std_20)
yanes_enable_assertions(audio_compare_tests)
yanes_warnings(audio_compare_tests)
add_test(NAME audio_compare_tests COMMAND audio_compare_tests $<TARGET_FILE:yanes-audio-compare>)
add_executable(hardware_fm_tests tests/hardware_fm_tests.cpp src/hardware_fm.cpp)
target_include_directories(hardware_fm_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(hardware_fm_tests PRIVATE cxx_std_20)
target_link_libraries(hardware_fm_tests PRIVATE yanes_ymfm)
yanes_warnings(hardware_fm_tests)
yanes_enable_assertions(hardware_fm_tests)
add_test(NAME hardware_fm_tests COMMAND hardware_fm_tests)
add_executable(dsp_tests tests/dsp_tests.cpp)
target_compile_features(dsp_tests PRIVATE cxx_std_20)
yanes_enable_assertions(dsp_tests)
add_test(NAME dsp_tests COMMAND dsp_tests)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # Offscreen editor renders for layout review; not a test, since it needs an X display.
  add_executable(yanes-ui-preview tools/ui_preview.cpp)
  target_compile_features(yanes-ui-preview PRIVATE cxx_std_20)
  target_include_directories(yanes-ui-preview PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(yanes-ui-preview PRIVATE X11::X11 X11::Xrender PkgConfig::XFT)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(frontend_tests tests/frontend_tests.cpp src/hardware_fm.cpp)
  find_package(Fontconfig REQUIRED)
  target_compile_features(frontend_tests PRIVATE cxx_std_20)
  target_include_directories(frontend_tests PRIVATE "${clap_SOURCE_DIR}/include")
  target_link_libraries(frontend_tests PRIVATE yanes_ymfm X11::X11 X11::Xrender PkgConfig::XFT Fontconfig::Fontconfig)
  yanes_warnings(frontend_tests)
  yanes_enable_assertions(frontend_tests)
  add_test(NAME frontend_tests COMMAND frontend_tests)
  set_tests_properties(frontend_tests PROPERTIES TIMEOUT 10)
endif()

add_executable(ui_tests tests/ui_tests.cpp)
target_compile_features(ui_tests PRIVATE cxx_std_20)
yanes_warnings(ui_tests)
yanes_enable_assertions(ui_tests)
target_include_directories(ui_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
add_test(NAME ui_tests COMMAND ui_tests)

add_executable(plugin_smoke tests/plugin_smoke.cpp)
target_compile_features(plugin_smoke PRIVATE cxx_std_20)
yanes_enable_assertions(plugin_smoke)
target_include_directories(plugin_smoke PRIVATE "${clap_SOURCE_DIR}/include")
target_link_libraries(plugin_smoke PRIVATE ${CMAKE_DL_LIBS})
add_test(NAME plugin_smoke COMMAND plugin_smoke $<TARGET_FILE:yanes_clap>)

# Behavioural suites that drive the built plug-in through the public CLAP interfaces.
foreach(suite note_tests state_tests bank_tests preset_tests)
  add_executable(${suite} tests/${suite}.cpp)
  target_compile_features(${suite} PRIVATE cxx_std_20)
  yanes_warnings(${suite})
  yanes_enable_assertions(${suite})
  target_include_directories(${suite} PRIVATE "${clap_SOURCE_DIR}/include")
  target_link_libraries(${suite} PRIVATE ${CMAKE_DL_LIBS})
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
  add_test(NAME reaper_nes_song COMMAND
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_reaper_nes_song.sh)
  set_tests_properties(reaper_nes_song PROPERTIES TIMEOUT 120 ENVIRONMENT
    "REAPER=${REAPER_EXECUTABLE};YANES_CLAP=$<TARGET_FILE:yanes_clap>;YANES_AUDIO_COMPARE=$<TARGET_FILE:yanes-audio-compare>")
endif()
add_test(NAME ymfm_opm_register_render COMMAND yanes-register-render ym2151
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/ym2151_tone.reg
  ${CMAKE_CURRENT_BINARY_DIR}/ym2151_tone.wav)
foreach(chip ym2203 ym2608 ym3812 ymf262)
  set(register_fixture ym2612)
  if(chip STREQUAL "ym3812" OR chip STREQUAL "ymf262")
    set(register_fixture ${chip})
  endif()
  add_test(NAME ${chip}_register_render COMMAND yanes-register-render ${chip}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/${register_fixture}_tone.reg
    ${CMAKE_CURRENT_BINARY_DIR}/${chip}_tone.wav)
endforeach()

# Producing a WAV header is insufficient: every chip render must contain valid audio.
foreach(chip ym2203 ym2608 ym2612 ym2151 ym3812 ymf262)
  if(chip STREQUAL "ym2612")
    set(render_test ymfm_register_render)
  elseif(chip STREQUAL "ym2151")
    set(render_test ymfm_opm_register_render)
  else()
    set(render_test ${chip}_register_render)
  endif()
  set_tests_properties(${render_test} PROPERTIES FIXTURES_SETUP ${chip}_audio)
  add_test(NAME ${chip}_audio_non_silent COMMAND yanes-audio-compare
    ${CMAKE_CURRENT_BINARY_DIR}/${chip}_tone.wav ${CMAKE_CURRENT_BINARY_DIR}/${chip}_tone.wav 0.9999)
  set_tests_properties(${chip}_audio_non_silent PROPERTIES FIXTURES_REQUIRED ${chip}_audio)
endforeach()
set_tests_properties(audio_compare PROPERTIES FIXTURES_REQUIRED ym2612_audio)

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

# Full chip-audio oracle suite. Furnace 0.6.8.3 cannot load these INF2 fixtures;
# use git furnace >= dev250. Pass -DYANES_FURNACE_EXECUTABLE=... or rely on PATH.
set(YANES_FURNACE_EXECUTABLE "" CACHE FILEPATH "Furnace executable for the 21-chip audio parity suite")
if(NOT YANES_FURNACE_EXECUTABLE)
  find_program(YANES_FURNACE_EXECUTABLE furnace)
endif()
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
# Preset gate selects a score using NES_ROM_DIR (or the script's default ROM directory).
if(UNIX)
  add_test(NAME nes_preset_gate COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_nes_preset_gate.sh)
  set_tests_properties(nes_preset_gate PROPERTIES TIMEOUT 120 ENVIRONMENT
    "YANES_CLAP=$<TARGET_FILE:yanes_clap>;YANES_NES_ROM_EMU=$<TARGET_FILE:yanes-nes-rom-emu>;YANES_AUDIO_COMPARE=$<TARGET_FILE:yanes-audio-compare>;YANES_PARITY_COMPARE=$<TARGET_FILE:yanes-parity-compare>")
endif()
