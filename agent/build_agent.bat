@echo off
setlocal
rem Build the device agent for arm64-v8a.
rem Set ANDROID_NDK to your NDK root (r26d or newer tested layout), or pass it:
rem   build_agent.bat C:\Android\Sdk\ndk\26.3.11579264

if not "%~1"=="" set ANDROID_NDK=%~1
if "%ANDROID_NDK%"=="" (
    echo [s2sc] ANDROID_NDK is not set.
    exit /b 1
)

set TC=%ANDROID_NDK%\build\cmake\android.toolchain.cmake

for %%A in (arm64-v8a x86_64) do (
    cmake -S "%~dp0." -B "%~dp0build-%%A" ^
        -DCMAKE_TOOLCHAIN_FILE="%TC%" ^
        -DANDROID_ABI=%%A ^
        -DANDROID_PLATFORM=android-21 ^
        -DANDROID_STL=c++_static ^
        -DCMAKE_BUILD_TYPE=Release
    if errorlevel 1 exit /b 1
    cmake --build "%~dp0build-%%A" --parallel
    if errorlevel 1 exit /b 1
)

echo.
echo [s2sc] arm64 : %~dp0build-arm64-v8a\s2sc_agent
echo [s2sc] x86_64: %~dp0build-x86_64\s2sc_agent
