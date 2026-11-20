@echo off
setlocal
rem Build the PC side (ImGui + DX11 + adb transport).
rem Requires: Visual Studio 2022 (MSVC x64), CMake 3.21+, git on PATH (imgui is fetched).

set ROOT=%~dp0
cmake -S "%ROOT%." -B "%ROOT%build" -G "Visual Studio 17 2022" -A x64
if errorlevel 1 exit /b 1
cmake --build "%ROOT%build" --config Release
if errorlevel 1 exit /b 1

echo.
echo [s2sc] exe: %ROOT%build\pc\Release\s2sc.exe
