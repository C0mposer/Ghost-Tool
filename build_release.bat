@echo off
REM USB-only release build. Clears NETWORK/DEBUG_UI if set in this shell
REM (e.g. after running build_debug.bat in the same cmd session).
cd /d "%~dp0"
set "RELEASE=1"
set "NETWORK="
set "DEBUG_UI="
set "GHOST_LOADER_DEBUG_UI="
bash build.sh
