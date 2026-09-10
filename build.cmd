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
  pushd "%BUILDDIR%"
  "%CTEST%" --output-on-failure
  set "RC=!ERRORLEVEL!"
  popd
  exit /b !RC!
)

echo.
echo Built %CONFIG% -^> %BUILDDIR%
endlocal
