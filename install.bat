@echo off
rem Builds g13map, copies it to %LOCALAPPDATA%\g13map, starts it at every login
rem and starts it now. Rerun after editing g13.ini to apply the changes.
setlocal
cd /d "%~dp0"
set "DEST=%LOCALAPPDATA%\g13map"

rem Ask politely first, so the tray icon gets removed, then make sure it's gone.
taskkill /im g13map-bg.exe >nul 2>&1
ping -n 2 127.0.0.1 >nul
taskkill /im g13map-bg.exe /f >nul 2>&1
taskkill /im g13map.exe /f >nul 2>&1

call "%~dp0build.bat" || exit /b 1

if not exist "%DEST%" mkdir "%DEST%"
copy /y build\g13map-bg.exe "%DEST%\" >nul || exit /b 1
copy /y g13.ini "%DEST%\" >nul || exit /b 1

reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v g13map /t REG_SZ /d "\"%DEST%\g13map-bg.exe\"" /f >nul || exit /b 1
start "" "%DEST%\g13map-bg.exe"

echo Installed to %DEST%. g13map now runs in the background and starts at login.
echo Log file: %DEST%\g13map.log
