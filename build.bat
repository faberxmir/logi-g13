@echo off
setlocal
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT (
    echo Visual Studio C++ build tools not found.
    exit /b 1
)
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

if not exist build mkdir build
set "FLAGS=/nologo /W4 /O2 /DUNICODE /D_UNICODE /Fobuild\ /Fdbuild\"
set "LIBS=hid.lib setupapi.lib user32.lib gdi32.lib shell32.lib"

cl %FLAGS% /Febuild\test_decode.exe tests\test_decode.c %LIBS% || exit /b 1
build\test_decode.exe || exit /b 1

set "VIGEM=third_party\ViGEmClient"
cl /nologo /w /O2 /EHsc /c /DUNICODE /D_UNICODE /I%VIGEM%\include /Fobuild\ViGEmClient.obj %VIGEM%\src\ViGEmClient.cpp || exit /b 1
cl %FLAGS% /I%VIGEM%\include /Febuild\g13map.exe src\g13map.c build\ViGEmClient.obj %LIBS% || exit /b 1
rem Same program with no console window, for running in the background.
cl %FLAGS% /I%VIGEM%\include /Febuild\g13map-bg.exe src\g13map.c build\ViGEmClient.obj %LIBS% /link /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup || exit /b 1
copy /y g13.ini build\g13.ini >nul
echo Built build\g13map.exe
