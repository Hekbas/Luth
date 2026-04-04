@echo off
setlocal

:: 1. Dynamically locate the latest MSBuild.exe installation
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do (
    set "MSBUILD_PATH=%%i"
)

:: 2. Fail gracefully if not found
if not defined MSBUILD_PATH (
    echo ERROR: MSBuild could not be found. Make sure Visual Studio is installed.
    pause
    exit /b 1
)

:: 3. Execute the build
pushd ..\..
"%MSBUILD_PATH%" Luth.sln /p:Configuration=Debug /p:Platform=x64
popd

pause