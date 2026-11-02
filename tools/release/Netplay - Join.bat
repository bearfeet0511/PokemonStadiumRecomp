@echo off
rem Joins a 2-player netplay session; you are player 2.
cd /d "%~dp0"
set HOST=
set /p HOST=Host's address (e.g. 203.0.113.5, or address:port):
if "%HOST%"=="" exit /b
echo %HOST% | find ":" >nul
if errorlevel 1 set HOST=%HOST%:7311
set PSR_NETPLAY=join:%HOST%
start "" "%~dp0PokemonStadiumRecomp.exe"
