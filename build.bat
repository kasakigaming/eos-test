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

echo [1/5] proxy DLL
cl /nologo /LD /O2 /EHsc /Fobuild\ src\main.c /link /DLL /OUT:EOSSDK-Win64-Shipping.dll /IMPLIB:build\proxy.lib User32.lib
if errorlevel 1 goto :fail

echo [2/5] payload packer
cl /nologo /O2 /Fobuild\ /Fe:build\pack.exe tools\pack.c
if errorlevel 1 goto :fail

echo [3/5] auth backend payloads
rem An empty or missing payload folder is fine: it packs to an empty archive
rem and the installer reports that backend as not bundled. See the _README.txt
rem in each folder for which files to drop in.
build\pack.exe installer\payload\gbe_fork   build\gbe_fork.pak
if errorlevel 1 goto :fail
build\pack.exe installer\payload\uc-online2 build\uc-online2.pak
if errorlevel 1 goto :fail

echo [3b/5] anti-cheat launcher stand-in
rem Windows subsystem so replacing a game's launcher does not flash a console.
cl /nologo /O2 /Fobuild\ /Fe:build\eos-proxy-launcher.exe installer\launcher.c User32.lib /link /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup
if errorlevel 1 goto :fail

echo [4/5] installer resources (embedding the DLL and the payloads)
rc /nologo /I . /I installer /I build /fo build\setup.res installer\setup.rc
if errorlevel 1 goto :fail

echo [5/5] installer exe
rem Urlmon/Wininet: downloading a backend release at install time, see fetch.h.
cl /nologo /O2 /Fobuild\ /Fe:eos-proxy-setup.exe installer\setup.c build\setup.res /link User32.lib Advapi32.lib Shell32.lib Urlmon.lib Wininet.lib
if errorlevel 1 goto :fail

echo.
echo Done:
echo   EOSSDK-Win64-Shipping.dll   proxy, drop it in the game folder by hand
echo   eos-proxy-setup.exe         installer with the DLL and payloads baked in
exit /b 0

:nocompiler
echo ERROR: MSVC not found. Install VS Build Tools with the C++ workload,
echo        or run this from a "x64 Native Tools Command Prompt".
exit /b 1

:fail
echo.
echo BUILD FAILED
exit /b 1
