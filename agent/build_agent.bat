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

set OUT=%~dp0build
cmake -S "%~dp0." -B "%OUT%" ^
    -G Ninja ^
    -DCMAKE_TOOLCHAIN_FILE="%ANDROID_NDK%\build\cmake\android.toolchain.cmake" ^
    -DANDROID_ABI=arm64-v8a ^
    -DANDROID_PLATFORM=android-21 ^
    -DANDROID_STL=c++_static ^
    -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1

cmake --build "%OUT%" --config Release
if errorlevel 1 exit /b 1

echo.
echo [s2sc] agent: %OUT%\s2sc_agent
