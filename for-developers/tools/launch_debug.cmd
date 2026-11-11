@echo off
rem Launches the latest IntelFix build with the debug server for measurements (agent use).
set EDEN_NX2_DEBUG_SERVER=127.0.0.1:6545
set EDEN_NX2_DEBUG_DIR=C:\work\Fritchii-intel\_tools\debugdump
set EDEN_NX2_GPU_PROFILE=1
set EDEN_DKB_FRAME_TRACE=1
set EDEN_NX2_PRECOMPILE=0
start "" "%~1\lichii.exe" -g "%GAME%"
