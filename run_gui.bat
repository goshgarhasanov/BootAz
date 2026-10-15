@echo off
rem BootAz desktop app (Windows). Builds with CMake + Qt 6 on first use, then starts it.
rem BootAz asks for Administrator rights when it starts (needed to write drives).
cd /d "%~dp0"
if not exist build\BootAz.exe (
    echo BootAz: building...
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || goto :fail
    cmake --build build || goto :fail
    windeployqt build\BootAz.exe >nul 2>&1
)
start "" build\BootAz.exe %*
exit /b 0
:fail
echo Build failed. Install Qt 6 (MinGW or MSVC), CMake and Ninja, then run this from a "Qt 6 command prompt".
pause
exit /b 1
