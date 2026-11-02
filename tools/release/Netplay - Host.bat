@echo off
rem Hosts a 2-player netplay session; you are player 1.
rem Your router must forward TCP port 7311 to this PC (see NETPLAY.txt).
cd /d "%~dp0"
set PSR_NETPLAY=host:7311
start "" "%~dp0PokemonStadiumRecomp.exe"
