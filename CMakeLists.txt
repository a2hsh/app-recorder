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
