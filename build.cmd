@echo off
REM apprecorder build driver.
REM Everything needed ships with VS 2022 Build Tools; nothing to install.
REM
REM   build.cmd              -> configure + build Debug
REM   build.cmd Release      -> configure + build Release
REM   build.cmd Debug spikes -> also build spike/ probes
REM   build.cmd test         -> build Debug then run ctest

setlocal enabledelayedexpansion

REM ---------------------------------------------------------------------------
REM FIND VISUAL STUDIO RATHER THAN ASSUMING WHERE IT IS.
REM
REM This used to hardcode the Build Tools path, which worked on exactly one
REM machine. It breaks on a second laptop that installed somewhere else, on a
REM full Visual Studio install (Community/Professional live under a different
REM folder entirely), and on a CI runner, where the toolchain is present but
REM nowhere near that path.
REM
REM vswhere ships with every Visual Studio 2017+ installer and its own location
REM IS fixed by Microsoft, so it is the one path worth hardcoding. Asking it for
REM an install that actually has the C++ tools also means a machine with only
REM the C# workload fails here, with a sentence, rather than deep inside a
REM compile.
REM ---------------------------------------------------------------------------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo ERROR: vswhere.exe not found. Install Visual Studio 2022 Build Tools:
  echo   winget install --id Microsoft.VisualStudio.2022.BuildTools -e ^
--override "--quiet --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
  exit /b 1
)

set "VSROOT="
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * ^
  -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 ^
  -property installationPath`) do set "VSROOT=%%I"

if not defined VSROOT (
  echo ERROR: no Visual Studio install with the C++ tools was found.
  echo Add the workload:
  echo   winget install --id Microsoft.VisualStudio.2022.BuildTools -e ^
--override "--quiet --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
  exit /b 1
)

set "VCVARS=%VSROOT%\VC\Auxiliary\Build\vcvars64.bat"
set "CMAKE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "CTEST=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
set "NINJA=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

if not exist "%VCVARS%" (
  echo ERROR: vcvars64.bat not found under "%VSROOT%"
  exit /b 1
)

REM CMake and Ninja ship inside Visual Studio, but a CI image or a trimmed
REM install may not have them there. Fall back to PATH before giving up, so a
REM runner that provides its own is not refused for having a different layout.
if not exist "%CMAKE%" for %%I in (cmake.exe) do set "CMAKE=%%~$PATH:I"
if not exist "%CTEST%" for %%I in (ctest.exe) do set "CTEST=%%~$PATH:I"
if not exist "%NINJA%" for %%I in (ninja.exe) do set "NINJA=%%~$PATH:I"

if not exist "%CMAKE%" ( echo ERROR: cmake.exe not found. & exit /b 1 )
if not exist "%NINJA%" ( echo ERROR: ninja.exe not found. & exit /b 1 )

set "CONFIG=Debug"
set "SPIKES=OFF"
set "RUNTESTS=0"

for %%A in (%*) do (
  if /I "%%A"=="Release" set "CONFIG=Release"
  if /I "%%A"=="Debug"   set "CONFIG=Debug"
  if /I "%%A"=="spikes"  set "SPIKES=ON"
  if /I "%%A"=="test"    set "RUNTESTS=1"
)

set "BUILDDIR=%~dp0build\%CONFIG%"

call "%VCVARS%" >nul || exit /b 1

"%CMAKE%" -S "%~dp0." -B "%BUILDDIR%" -G Ninja ^
  -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
  -DCMAKE_BUILD_TYPE=%CONFIG% ^
  -DBUILD_SPIKES=%SPIKES% || exit /b 1

"%CMAKE%" --build "%BUILDDIR%" || exit /b 1

if "%RUNTESTS%"=="1" (
  REM -------------------------------------------------------------------------
  REM HOW MANY WORKERS: ASK THE MACHINE, DO NOT ASSUME THE AUTHOR'S.
  REM
  REM ctest is serial by default, which meant 41 binaries queueing one at a time
  REM on a many-core machine for no reason: 60s serial against 24s in parallel,
  REM with identical results. Most suites are pure computation and finish in
  REM hundredths of a second; they were simply waiting their turn.
  REM
  REM But the count was then hardcoded at 8, and 8 is a property of THIS
  REM workstation (24 cores), not of the job. A GitHub runner has 4, so 8 was
  REM two processes per core, several of them holding real windows, and every
  REM timing assumption in the tree got to absorb the difference.
  REM
  REM So: one worker per core, capped at 8 and floored at 2. The cap is measured
  REM -- beyond 8 the wall clock is set by the single longest suite and more
  REM workers buy nothing (24.2s at -j 8 against 23.4s at -j 16 here) -- and the
  REM floor keeps a single-core VM from running serially by accident.
  REM
  REM The windowed suites additionally hold a RESOURCE_LOCK (CMakeLists.txt), so
  REM raising this number never puts two real frames on the desktop at once.
  REM -------------------------------------------------------------------------
  set "JOBS=%NUMBER_OF_PROCESSORS%"
  if not defined JOBS set "JOBS=4"
  set /a JOBS=JOBS+0
  if !JOBS! GTR 8 set "JOBS=8"
  if !JOBS! LSS 2 set "JOBS=2"

  pushd "%BUILDDIR%"
  REM A skipped case appends a line here (tests/test_runner.h). Start each run
  REM from an empty file so what is printed below is THIS run's skips.
  if exist "test-skips.log" del /q "test-skips.log"
  echo Running ctest with !JOBS! workers ^(this machine reports %NUMBER_OF_PROCESSORS% processors^)
  "%CTEST%" --output-on-failure -j !JOBS!
  set "RC=!ERRORLEVEL!"
  REM SAY WHAT DID NOT RUN. ctest keeps the output of a suite that failed and
  REM discards the rest, so a case that skipped for want of hardware -- an audio
  REM engine, a window station, UI Automation -- was invisible in a green run,
  REM which is precisely the shape of "passed" that means nothing.
  if exist "test-skips.log" (
    echo.
    echo Cases SKIPPED in this run -- these did NOT test anything:
    type "test-skips.log"
  )
  popd
  exit /b !RC!
)

echo.
echo Built %CONFIG% -^> %BUILDDIR%
endlocal
