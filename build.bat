@echo off
setlocal enabledelayedexpansion

cd /d "%~dp0"

set "VS_PATH="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" set "VS_PATH=%%i"
    )
)
if not defined VS_PATH (
    if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
        set "VS_PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\2019\BuildTools"
    )
)
if not defined VS_PATH (
    echo [ERROR] Could not locate MSVC x64 Build Tools vcvars64.bat.
    exit /b 1
)

call "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo [ERROR] Failed to initialize vcvars64.bat.
    exit /b 1
)

if not exist "build" mkdir "build"

if not exist "res\TapBPM.ico" (
    echo Generating icon...
    powershell -NoProfile -ExecutionPolicy Bypass -File "scripts\gen_icon.ps1"
    if errorlevel 1 exit /b 1
)

echo Compiling resources...
rc /nologo /fo "build\TapBPM.res" "res\TapBPM.rc"
if errorlevel 1 exit /b 1

echo Building TapBPM.exe...
cl /nologo /std:c++17 /O1 /GL /MT /W4 /WX /EHs-c- /GR- /DUNICODE /D_UNICODE ^
   /Fo:"build\\" /Fe:"TapBPM.exe" ^
   "src\bpm_engine.cpp" "src\main.cpp" "build\TapBPM.res" ^
   /link /SUBSYSTEM:WINDOWS /LTCG /OPT:REF /OPT:ICF /MANIFEST:NO ^
   user32.lib gdi32.lib gdiplus.lib dwmapi.lib advapi32.lib
if errorlevel 1 exit /b 1

echo Building test_bpm_engine.exe...
cl /nologo /std:c++17 /O1 /MT /W4 /WX /EHs-c- /GR- /DUNICODE /D_UNICODE ^
   /Fo:"build\\" /Fe:"build\test_bpm_engine.exe" ^
   "src\bpm_engine.cpp" "tests\test_bpm_engine.cpp"
if errorlevel 1 exit /b 1

if /I "%~1"=="test" (
    echo Running unit tests...
    "build\test_bpm_engine.exe"
    if errorlevel 1 exit /b 1
)

echo Build complete: TapBPM.exe
exit /b 0
