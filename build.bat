@echo off
setlocal
cd /d "%~dp0"

rem Prefer the MinGW-w64 installation that is already present on this machine.
if exist "D:\mingw64\bin\g++.exe" set "PATH=D:\mingw64\bin;%PATH%"
if exist "C:\msys64\mingw64\bin\g++.exe" set "PATH=C:\msys64\mingw64\bin;%PATH%"

where g++ >nul 2>nul
if errorlevel 1 (
  echo [fastbootCmds] g++ not found.
  echo Install 64-bit MinGW-w64, or add its bin directory to PATH.
  echo Example:
  echo   set PATH=D:\mingw64\bin;%%PATH%%
  pause
  exit /b 1
)

echo Using:
g++ --version | findstr /i "g++"
echo.

g++ -std=c++17 -O2 -Wall -Wextra -Werror -Isrc ^
  src\main.cpp src\winusb_fastboot.cpp src\fastboot_client.cpp src\usb_path_windows.cpp ^
  -lsetupapi -lcfgmgr32 -lwinusb -lshell32 ^
  -static -static-libgcc -static-libstdc++ -o fastbootCmds.exe

if errorlevel 1 (
  echo.
  echo Build failed.
  pause
  exit /b 1
)

echo.
echo Built: %CD%\fastbootCmds.exe
echo Copy it to D:\platform-tools if needed:
echo   copy /Y fastbootCmds.exe D:\platform-tools\
pause
