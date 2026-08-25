@echo off
REM apprecorder build driver.
REM Everything needed ships with VS 2022 Build Tools; nothing to install.
REM
REM   build.cmd              -> configure + build Debug
REM   build.cmd Release      -> configure + build Release
REM   build.cmd Debug spikes -> also build spike/ probes
REM   build.cmd test         -> build Debug then run ctest

setlocal enabledelayedexpansion

set "VSROOT=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
set "VCVARS=%VSROOT%\VC\Auxiliary\Build\vcvars64.bat"
set "CMAKE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "CTEST=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
set "NINJA=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

if not exist "%VCVARS%" (
  echo ERROR: vcvars64.bat not found at "%VCVARS%"
  exit /b 1
)

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
