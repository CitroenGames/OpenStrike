@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "ROOT=%~dp0"
set "SHADER_DIR=%ROOT%Content\shaders"
set "OUT_DIR=%SHADER_DIR%\compiled\dx12"
set "KITS_BIN=%ProgramFiles(x86)%\Windows Kits\10\bin"
set "FXC="

if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"

where fxc.exe >nul 2>nul
if not errorlevel 1 set "FXC=fxc.exe"

if not defined FXC (
    for /d %%D in ("!KITS_BIN!\*") do (
        if exist "%%~fD\x64\fxc.exe" set "FXC=%%~fD\x64\fxc.exe"
    )
)

if not defined FXC (
    echo fxc.exe was not found. Install the Windows SDK or run this from a Developer Command Prompt.
    exit /b 1
)

call :compile "world_material.hlsl" "VSMain" "vs_5_0" "world_material.vs.cso" || exit /b 1
call :compile "world_material.hlsl" "PSMain" "ps_5_0" "world_material.ps.cso" || exit /b 1
call :compile "skybox.hlsl" "VSMain" "vs_5_0" "skybox.vs.cso" || exit /b 1
call :compile "skybox.hlsl" "PSMain" "ps_5_0" "skybox.ps.cso" || exit /b 1

echo Shaders compiled to "%OUT_DIR%".
exit /b 0

:compile
echo Compiling %~1 %~2 -^> %~4
"%FXC%" /nologo /T %~3 /E %~2 /Fo "%OUT_DIR%\%~4" "%SHADER_DIR%\%~1"
exit /b %errorlevel%
