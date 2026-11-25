@echo off
setlocal
chcp 65001 >nul
title spv_replace portability check
echo.
echo Building spv_check.exe (needs Visual Studio C++ build tools, no Vulkan SDK)...
echo.

rem --- find a Visual Studio installation (any edition, 2019/2022+) ---
set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "VSPATH=%%i"
if not defined VSPATH goto tryprompt
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
goto build

:tryprompt
where cl.exe >nul 2>&1
if errorlevel 1 (
    echo ERROR: Visual Studio C++ tools were not found.
    echo Install "Desktop development with C++" in Visual Studio, then run this file again.
    pause
    exit /b 1
)

:build
cl /nologo /std:c++17 /utf-8 /EHsc /O2 /MT spv_check.cpp /link /SUBSYSTEM:CONSOLE /OUT:spv_check.exe
if errorlevel 1 (
    echo.
    echo ERROR: compilation failed. Send this window text.
    pause
    exit /b 1
)
echo.
echo Built spv_check.exe
del /q spv_check.obj >nul 2>&1
spv_check.exe %*
exit /b %errorlevel%
