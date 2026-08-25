cmake_minimum_required(VERSION 3.20)
project(apprecorder C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT MSVC)
  message(FATAL_ERROR "apprecorder targets MSVC. See build.cmd.")
endif()

# Warnings are errors. This codebase hand-writes COM vtables; sloppiness is not
# survivable here.
add_compile_options(/W4 /WX /permissive-)
add_compile_definitions(
  COBJMACROS                # C-style IAudioClient_*(This, ...) accessors
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  UNICODE _UNICODE
  _CRT_SECURE_NO_WARNINGS
)

# core/registry.c holds the static action table (design 3.3). Each entry is
# guarded by APR_HAVE_ACTION_<ID>, defined here from the presence of the source
# file, so an encoder that is half written cannot break the link for everyone
# else and a finished one needs no build-system edit.
foreach(_action wav mp3 ogg m4a)
  if(EXISTS ${CMAKE_SOURCE_DIR}/src/actions/action_${_action}.c)
    string(TOUPPER ${_action} _ACTION)
    add_compile_definitions(APR_HAVE_ACTION_${_ACTION})
  endif()
endforeach()

# Size goals: static CRT, no runtime redistributable required.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(CMAKE_BUILD_TYPE STREQUAL "Release")
  add_compile_options(/O1 /GL /Gy /Gw)
  add_link_options(/LTCG /OPT:REF /OPT:ICF)
endif()

include_directories(${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)

# ---------------------------------------------------------------------------
# Core library: everything except the UI entry point. Tests link against this,
# which is why the capture layer must sit behind an interface a fake can
# implement (see design section 4.3).
# ---------------------------------------------------------------------------
file(GLOB CORE_SRC
  ${CMAKE_SOURCE_DIR}/src/core/*.c
  ${CMAKE_SOURCE_DIR}/src/capture/*.c
  ${CMAKE_SOURCE_DIR}/src/actions/*.c
  ${CMAKE_SOURCE_DIR}/src/session/*.c
  ${CMAKE_SOURCE_DIR}/src/platform/*.c
)

if(CORE_SRC)
  add_library(apprecorder_core STATIC ${CORE_SRC})
  target_link_libraries(apprecorder_core PUBLIC
    ole32 oleaut32 uuid avrt mmdevapi winmm
  )
endif()

# ---------------------------------------------------------------------------
# MP3: vendored libmp3lame, built static so there is no DLL to ship.
# LGPL-2.0-or-later; see vendor/lame/PROVENANCE.md. Warnings are relaxed inside
# that target only -- /W4 /WX above still governs everything we write.
# ---------------------------------------------------------------------------
add_subdirectory(${CMAKE_SOURCE_DIR}/vendor/lame)
if(TARGET apprecorder_core)
  target_link_libraries(apprecorder_core PUBLIC lame)
endif()

# ---------------------------------------------------------------------------
# Tests: one executable per test file, so a hang in one does not hide others.
# ---------------------------------------------------------------------------
enable_testing()
file(GLOB TEST_SRC ${CMAKE_SOURCE_DIR}/tests/test_*.c)
foreach(test_file ${TEST_SRC})
  get_filename_component(test_name ${test_file} NAME_WE)
  add_executable(${test_name} ${test_file})
  target_include_directories(${test_name} PRIVATE ${CMAKE_SOURCE_DIR}/tests)
  if(TARGET apprecorder_core)
    target_link_libraries(${test_name} PRIVATE apprecorder_core)
  endif()
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# ---------------------------------------------------------------------------
# Spikes: throwaway feasibility probes, built on demand only.
# ---------------------------------------------------------------------------
option(BUILD_SPIKES "Build throwaway feasibility spikes" OFF)
if(BUILD_SPIKES)
  file(GLOB SPIKE_SRC ${CMAKE_SOURCE_DIR}/spike/*.c)
  foreach(spike_file ${SPIKE_SRC})
    get_filename_component(spike_name ${spike_file} NAME_WE)
    add_executable(${spike_name} ${spike_file})
    target_link_libraries(${spike_name} PRIVATE ole32 oleaut32 uuid avrt mmdevapi)
  endforeach()
endif()

# ---------------------------------------------------------------------------
# i18n string catalog (localization layer).
#
# Two things this block has to get right:
#
# 1. src/i18n/*.c is not covered by the CORE_SRC glob above, so it is added
#    here rather than by editing that list while other agents are in it.
#
# 2. res/strings.rc is attached as an INTERFACE source, so every executable
#    that links apprecorder_core compiles the catalog into ITSELF. Resources
#    do not reliably survive a static library: link.exe pulls objects from a
#    .lib to resolve symbols, and a .res defines none, so the catalog can be
#    dropped from the final image with no diagnostic. An INTERFACE source
#    sidesteps the question entirely.
#
# Note for anyone touching the compile options above: CMake does NOT pass the
# directory-wide /W4 /WX /permissive- to rc.exe with this generator (verified
# from the actual command line -- rc.exe receives only -D/-I and /fo). If that
# ever changes, rc.exe rejects those switches outright and the fix is to guard
# them with $<COMPILE_LANGUAGE:C>, not to drop this block.
# ---------------------------------------------------------------------------
enable_language(RC)

if(TARGET apprecorder_core)
  file(GLOB I18N_SRC ${CMAKE_SOURCE_DIR}/src/i18n/*.c)
  target_sources(apprecorder_core PRIVATE ${I18N_SRC})
  target_sources(apprecorder_core INTERFACE ${CMAKE_SOURCE_DIR}/res/strings.rc)
endif()
