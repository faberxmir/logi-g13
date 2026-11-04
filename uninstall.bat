@echo off
rem Stops g13map, removes it from login startup and deletes the installed copy.
rem The ViGEmBus driver stays installed: winget uninstall ViGEm.ViGEmBus
setlocal
set "DEST=%LOCALAPPDATA%\g13map"

rem Ask politely first, so the tray icon gets removed, then make sure it's gone.
taskkill /im g13map-bg.exe >nul 2>&1
ping -n 2 127.0.0.1 >nul
taskkill /im g13map-bg.exe /f >nul 2>&1
reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v g13map /f >nul 2>&1
if exist "%DEST%" rmdir /s /q "%DEST%"
echo g13map removed.
