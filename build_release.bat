@echo off
REM
REM
cd /d "%~dp0"
set "RELEASE=1"
set "NETWORK="
set "DEBUG_UI="
set "GHOST_LOADER_DEBUG_UI="
bash build.sh
