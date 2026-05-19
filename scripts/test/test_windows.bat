@echo off
setlocal

:: Run LuthTests.exe in Debug or DebugASan config. Default: DebugASan (the interesting one).
:: Usage:
::   test_windows.bat              -> Debug
::   test_windows.bat DebugASan    -> ASan-instrumented (recommended for stress sub-tasks)
::   test_windows.bat Debug --test-case=*[smoke]*   -> filter

set "CFG=%1"
if "%CFG%"=="" set "CFG=Debug"
shift

:: 1. Locate the latest MSVC install via vswhere
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.Component.MSBuild -property installationPath`) do (
    set "VS_INSTALL=%%i"
)

if not defined VS_INSTALL (
    echo ERROR: Visual Studio install not found.
    exit /b 1
)

:: 2. Find the MSVC toolset version (newest under VC\Tools\MSVC\)
for /f "usebackq tokens=*" %%i in (`dir /b /ad /o-n "%VS_INSTALL%\VC\Tools\MSVC\"`) do (
    if not defined MSVC_VER set "MSVC_VER=%%i"
)

if not defined MSVC_VER (
    echo ERROR: MSVC toolset not found.
    exit /b 1
)

set "ASAN_DIR=%VS_INSTALL%\VC\Tools\MSVC\%MSVC_VER%\bin\Hostx64\x64"
set "PATH=%ASAN_DIR%;%PATH%"

:: 3. Run from repo root
pushd ..\..
"bin\windows-x86_64\%CFG%\LuthTests\LuthTests.exe" %1 %2 %3 %4 %5 %6 %7 %8 %9
set "RC=%ERRORLEVEL%"
popd

exit /b %RC%
