@echo off
setlocal enabledelayedexpansion

rem Builds the proxy DLL and eos-proxy-setup.exe, which embeds that DLL.
rem Run from a VS developer prompt, or from anywhere: vcvars is located below.

where cl >nul 2>nul
if not errorlevel 1 goto :build

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :nocompiler
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH goto :nocompiler
rem vcvars probes for optional components and prints a harmless stderr line.
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
where cl >nul 2>nul
if errorlevel 1 goto :nocompiler

:build
if not exist build mkdir build

echo [1/3] proxy DLL
cl /nologo /LD /O2 /EHsc /Fobuild\ src\main.c /link /DLL /OUT:EOSSDK-Win64-Shipping.dll /IMPLIB:build\proxy.lib User32.lib
if errorlevel 1 goto :fail

echo [2/3] installer resources (embedding the DLL)
rc /nologo /I . /I installer /fo build\setup.res installer\setup.rc
if errorlevel 1 goto :fail

echo [3/3] installer exe
cl /nologo /O2 /Fobuild\ /Fe:eos-proxy-setup.exe installer\setup.c build\setup.res /link User32.lib Advapi32.lib Shell32.lib
if errorlevel 1 goto :fail

echo.
echo Done:
echo   EOSSDK-Win64-Shipping.dll   proxy, drop it in the game folder by hand
echo   eos-proxy-setup.exe         installer with the DLL baked in
exit /b 0

:nocompiler
echo ERROR: MSVC not found. Install VS Build Tools with the C++ workload,
echo        or run this from a "x64 Native Tools Command Prompt".
exit /b 1

:fail
echo.
echo BUILD FAILED
exit /b 1
