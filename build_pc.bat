@echo off
setlocal
rem Build the PC side (ImGui + DX11 + adb transport).
rem Requires: any Visual Studio with the C++ workload, CMake 3.21+, git on PATH
rem (imgui is fetched by CMake). No generator is pinned on purpose.

set ROOT=%~dp0
cmake -S "%ROOT%." -B "%ROOT%build" -A x64
if errorlevel 1 exit /b 1
cmake --build "%ROOT%build" --config Release
if errorlevel 1 exit /b 1

echo.
echo [s2sc] exe: %ROOT%build\bin\s2sc.exe
