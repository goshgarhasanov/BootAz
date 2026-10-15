@echo off
rem BootAz command line (Windows). Run it from an Administrator command prompt to write drives.
rem   run_cli.bat devices
rem   run_cli.bat write --device \\.\PhysicalDrive2 --image Win11.iso
chcp 65001 >nul
cd /d "%~dp0"
if not exist build\bootaz-cli.exe (
    echo BootAz: building...
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBOOTAZ_GUI=OFF || goto :fail
    cmake --build build --target bootaz-cli || goto :fail
)
build\bootaz-cli.exe %*
exit /b %errorlevel%
:fail
echo Build failed. Install CMake, Ninja and a C++20 compiler (MinGW-w64 or Visual Studio).
exit /b 1
