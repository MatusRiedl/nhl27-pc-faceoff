@echo off
rem Double-click to play NHL 27 online on the CrossCrease server. The first start asks for your online ID.
powershell -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0play-nhl27-online.ps1" %*
if errorlevel 1 pause
