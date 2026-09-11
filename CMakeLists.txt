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
foreach(_action wav mp3 ogg)
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
  # bcrypt and winhttp are for the updater (src/platform/update*.c). Both are
  # in-box Windows libraries, which is the whole reason ECDSA verification and
  # HTTPS cost nothing in vendor/ -- see AGENTS.md rule 8 and include/update.h.
  target_link_libraries(apprecorder_core PUBLIC
    ole32 oleaut32 uuid avrt mmdevapi winmm bcrypt winhttp
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
# OGG/Opus: vendored libogg + libopus, both static for the same reason.
# BSD-3-Clause -- see vendor/ogg/PROVENANCE.md and vendor/opus/PROVENANCE.md.
# Warnings are relaxed inside those two targets only.
#
# Ogg/Opus support costs 220.5 KB of the Release image, measured by building
# with and without src/actions/action_ogg.c. Unlike LAME's decoder there is no
# encoder-only slice of libopus to carve out: the two halves share the entropy
# coder, the MDCT, the FFT and the mode tables. See vendor/opus/CMakeLists.txt.
# ---------------------------------------------------------------------------
add_subdirectory(${CMAKE_SOURCE_DIR}/vendor/ogg)
add_subdirectory(${CMAKE_SOURCE_DIR}/vendor/opus)
if(TARGET apprecorder_core)
  target_link_libraries(apprecorder_core PUBLIC ogg opus)
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
  # A test must leave no trace on the machine running it. Several UI suites
  # build a real controller, which builds a real tray icon and fires real
  # shell notifications -- read aloud by a screen reader, on every run.
  # Setting this here rather than per-file means a new UI suite cannot forget.
  # NO_UPDATE for the same reason: a controller with a real signing key
  # compiled in starts a genuine HTTPS request and rewrites HKCU on the
  # machine running the suite.
  #
  # SKIP_LOG is where a skipped case says so somewhere ctest cannot throw
  # away. ctest keeps the output of a suite that FAILED and discards the rest,
  # so "SKIPPED: no audio engine on this machine" -- the one line that explains
  # why a green run proved less than it looks -- was invisible on CI. Every
  # suite appends to this one file and build.cmd prints it after the run. See
  # tests/test_runner.h.
  set_tests_properties(${test_name} PROPERTIES
    ENVIRONMENT "APPRECORDER_NO_TRAY=1;APPRECORDER_NO_UPDATE=1;APPRECORDER_SKIP_LOG=${CMAKE_BINARY_DIR}/test-skips.log")

  # ------------------------------------------------------------------------
  # ONE DESKTOP, ONE WINDOWED SUITE AT A TIME.
  #
  # There are 41 suites and most of them are pure computation: a ring buffer, a
  # resampler, a clock. Those are why the run is parallel at all, and they do
  # not interfere with anything.
  #
  # Nine of them are not like that. They create REAL top-level windows on the
  # REAL interactive desktop and then assert about focus, activation, z-order,
  # visibility and what UI Automation says -- and every one of those is a
  # property of the DESKTOP, which is a single shared resource that ctest
  # otherwise knows nothing about. Running eight of them at once is eight
  # processes taking turns at one input queue; the previous pass measured that
  # directly (91 failures in 360 concurrent runs of one walk, every one with
  # focus reading NULL) and fixed the worst of it structurally with
  # WS_EX_NOACTIVATE. This is the other half: RESOURCE_LOCK tells ctest these
  # suites are mutually exclusive, so it schedules them one at a time while
  # everything else keeps running alongside them.
  #
  # Matched by NAME, exactly as the UI link rule below is and for the same
  # reason: a suite that creates a window has to be called test_ui_* or named
  # here, and a new one cannot forget to opt in. test_discover is named because
  # it creates a window without being a UI suite.
  #
  # AND THEY GO FIRST. A lock turns those nine into one chain, and a chain is
  # the critical path: nothing else in the run is anywhere near as long. Left
  # to ctest's default order the chain started a few seconds in and the whole
  # run finished when the chain did, with the machine idle at the end.
  # A high COST makes ctest schedule them first, so the chain runs UNDER the
  # rest of the suite instead of after it. Measured, Release, this machine,
  # same 41 suites and the same lock: 19.7 s without the COST, 13.6 s with it
  # -- and 14.4-16.4 s was the figure before there was a lock at all.
  if(test_name MATCHES "^test_ui_" OR test_name STREQUAL "test_discover")
    set_tests_properties(${test_name} PROPERTIES RESOURCE_LOCK desktop COST 1000)
  endif()
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

# ---------------------------------------------------------------------------
# UI layer.
#
# A STATIC LIBRARY WITH NO ENTRY POINT, deliberately. WinMain in a library
# collides with every test's main(), and which front end owns the process is
# not this layer's decision -- apr_ui_app_run() is the entry a future WinMain
# calls.
#
# res/apprecorder.rc is an INTERFACE source for the same reason res/strings.rc
# is one on apprecorder_core: a .res defines no symbols, so link.exe may drop
# it when pulling objects out of a .lib, and the manifest would vanish with no
# diagnostic. /MANIFEST:NO goes with it because link.exe's own manifest tool
# REPLACES resource #1 in the finished image -- with both mechanisms active the
# generated manifest wins and ours is silently discarded, leaving a comctl32 v5,
# system-DPI-aware process. tests/test_ui_a11y.c asserts at runtime that the
# manifest actually took effect, so this cannot drift unnoticed.
#
# APR_HAVE_UI_CANVAS / APR_HAVE_UI_TREE_PANEL follow the action-registry
# pattern above: defined from the presence of the source file, so app.c calls
# the real pane factory once it exists and falls back to a named placeholder
# until then. A half-written canvas cannot break the link for anyone else.
# ---------------------------------------------------------------------------
file(GLOB UI_SRC ${CMAKE_SOURCE_DIR}/src/ui/*.c)
if(UI_SRC)
  add_library(apprecorder_ui STATIC ${UI_SRC})
  # comdlg32 for the file pickers in src/ui/dialogs.c; shell32 for the
  # notification area in src/ui/tray.c.
  target_link_libraries(apprecorder_ui PUBLIC
    apprecorder_core comctl32 uxtheme dwmapi oleacc gdi32 user32 shell32
    comdlg32
  )
  # rc.exe dependency scanning follows #include only, so a change to the
  # manifest itself does not rebuild the .res: the old manifest stays
  # embedded and the next run fails with "the side-by-side configuration is
  # incorrect" against a file you have already fixed. State the dependency.
  set_source_files_properties(${CMAKE_SOURCE_DIR}/res/apprecorder.rc
    PROPERTIES OBJECT_DEPENDS ${CMAKE_SOURCE_DIR}/res/apprecorder.manifest)
  target_sources(apprecorder_ui INTERFACE ${CMAKE_SOURCE_DIR}/res/apprecorder.rc)
  target_link_options(apprecorder_ui INTERFACE /MANIFEST:NO)

  foreach(_pane canvas tree_panel)
    if(EXISTS ${CMAKE_SOURCE_DIR}/src/ui/${_pane}.c)
      string(TOUPPER ${_pane} _PANE)
      target_compile_definitions(apprecorder_ui PRIVATE APR_HAVE_UI_${_PANE})
    endif()
  endforeach()

  # The test loop above links every test against apprecorder_core only. The UI
  # tests additionally need the UI library, and through it the manifest.
  #
  # Matched by NAME, not by a hand-written list. The list was a three-way merge
  # point: every agent adding a UI test had to edit the same four words in this
  # shared file, which is exactly the conflict the APR_HAVE_* pattern above
  # exists to avoid. tests/test_ui_*.c now links the UI library by virtue of
  # being called that, and adding one needs no build-system edit at all.
  foreach(test_file ${TEST_SRC})
    get_filename_component(_uitest ${test_file} NAME_WE)
    if(_uitest MATCHES "^test_ui_" AND TARGET ${_uitest})
      target_link_libraries(${_uitest} PRIVATE apprecorder_ui)
    endif()
  endforeach()

  # The windowed front end no longer has an executable of its own. There is one
  # image, src/app/main.c, built at the bottom of this file -- see the block
  # named "The one executable". apprecorder_ui still has no entry point of its
  # own, for the reason at the top of this section.
endif()

# ---------------------------------------------------------------------------
# Command-line front end.
#
# cli.c goes into apprecorder_core rather than into the executable, so that
# tests/test_cli.c can drive the parser, the resolver, the dry run and the
# record loop directly -- there is no process boundary between the suite and
# the command line, and no output to scrape.
#
# src/app/frontend.c is here for the same reason one level up: which front end
# a command line asks for is a rule, the rule has a table of cases, and a rule
# that lives in an executable is a rule no test can reach. In the library, it
# is one pure function tests/test_frontend.c drives directly.
#
# It is a separate glob for the same reason src/i18n is: the CORE_SRC list
# above is shared, and appending here rather than editing it keeps this change
# additive while other agents are in that file.
# ---------------------------------------------------------------------------
if(TARGET apprecorder_core)
  target_sources(apprecorder_core PRIVATE
    ${CMAKE_SOURCE_DIR}/src/cli/cli.c
    ${CMAKE_SOURCE_DIR}/src/app/frontend.c)
endif()

# ---------------------------------------------------------------------------
# THE ONE EXECUTABLE.
#
# apprecorder.exe (839 KB) and apprecorder_ui_app.exe (915 KB) were 1.75 MB of
# mostly identical bytes: each statically linked the whole core -- capture, the
# graph, the mixer, the resampler, the drift controller, three encoders, LAME,
# libogg and libopus -- and the command line's own code is a rounding error
# next to that. One image is ~990 KB, one file to hand to a friend, one name on
# PATH.
#
# WIN32 is not a preference. A CONSOLE image flashes a black window on every
# double-click, which is not a thing to ship; the subsystem is fixed in the PE
# header, so it has to be decided here, once, for both front ends. The cost is
# real and is documented at the top of include/frontend.h: a WINDOWS image is
# handed no standard handles by cmd.exe and is not waited for by PowerShell, so
# apprecorder.com (below) is the console front door that restores both.
# tests/test_frontend.c reads the subsystem byte out of the built image, so a
# well-meaning revert to CONSOLE fails the suite rather than the double-click.
#
# It links apprecorder_ui, which brings apprecorder_core, the manifest and
# /MANIFEST:NO with it. Without the UI layer there is no windowed front end to
# dispatch to, so there is no merged image either -- the same presence test the
# old windowed target used, moved down here with it.
# ---------------------------------------------------------------------------
if(TARGET apprecorder_ui AND EXISTS ${CMAKE_SOURCE_DIR}/src/app/main.c)
  add_executable(apprecorder WIN32 ${CMAKE_SOURCE_DIR}/src/app/main.c)
  target_link_libraries(apprecorder PRIVATE apprecorder_ui)

  # -------------------------------------------------------------------------
  # apprecorder.com -- the CONSOLE front door, beside the WINDOWS image.
  #
  # PATHEXT's default begins ".COM;.EXE", so this is what a shell finds when
  # somebody types "apprecorder", and being a console image it is handed the
  # real standard handles and passes them to the child. That is what restores
  # redirection, pipes and captured output, none of which a WINDOWS-subsystem
  # image gets from cmd.exe -- and it is what makes PowerShell wait, which it
  # otherwise does not. src/app/launcher.c has the measurements.
  #
  # NO CRT: /NODEFAULTLIB with its own entry point takes it from 132 KB to a
  # few. /GS- because the stack cookie lives in the CRT, and the same reason
  # forbids an automatic buffer big enough to need __chkstk.
  # -------------------------------------------------------------------------
  add_executable(apprecorder_com ${CMAKE_SOURCE_DIR}/src/app/launcher.c)
  set_target_properties(apprecorder_com PROPERTIES
    OUTPUT_NAME "apprecorder"
    SUFFIX ".com")
  # /GL- and /LTCG:OFF: this file DEFINES memset and memcpy (the optimiser
  # emits calls to them from ordinary loops, and there is no CRT here to
  # provide them), and whole-program optimisation refuses to let a translation
  # unit define a compiler helper -- C2268. The target is a few KB either way.
  target_compile_options(apprecorder_com PRIVATE /GS- /GL-)
  target_link_options(apprecorder_com PRIVATE
    /NODEFAULTLIB /ENTRY:apr_com_start /SUBSYSTEM:CONSOLE /MANIFEST:NO
    /LTCG:OFF)
  target_link_libraries(apprecorder_com PRIVATE kernel32)
  # It resolves its partner by swapping its own extension, so the two travel
  # together or neither works.
  add_dependencies(apprecorder_com apprecorder)

  # tests/test_frontend.c runs the real image: its subsystem byte, its exit
  # codes, and whether its output reaches a redirected pipe with a non-ASCII
  # path intact. None of that can be asserted from inside the process, so the
  # path is handed to the suite rather than guessed at.
  if(TARGET test_frontend)
    add_dependencies(test_frontend apprecorder apprecorder_com)
    target_compile_definitions(test_frontend PRIVATE
      APR_APP_EXE=L"$<TARGET_FILE:apprecorder>"
      APR_APP_COM=L"$<TARGET_FILE:apprecorder_com>")
    # A dispatch bug that opened a window instead of running a command line
    # would otherwise hang here forever. The suite bounds every wait itself;
    # this is the second net.
    set_tests_properties(test_frontend PROPERTIES TIMEOUT 120)
  endif()
endif()

# ---------------------------------------------------------------------------
# Session persistence.
#
# src/session/*.c is already covered by the CORE_SRC glob above; the only
# thing this block adds is the include path for the vendored JSON tokenizer,
# and it is PRIVATE because session_load.c is the single translation unit
# permitted to parse JSON (design section 9 names one owner for the format).
# jsmn is a header-only MIT tokenizer -- see vendor/jsmn/PROVENANCE.md for the
# checksum and for why JSMN_STRICT is load-bearing rather than tidy.
#
# Appended rather than folded into the include_directories() call at the top
# for the same reason the i18n and CLI blocks are separate: that list is
# shared, and an additive block cannot conflict with an agent editing it.
# ---------------------------------------------------------------------------
if(TARGET apprecorder_core)
  target_include_directories(apprecorder_core
    PRIVATE ${CMAKE_SOURCE_DIR}/vendor/jsmn)
endif()
