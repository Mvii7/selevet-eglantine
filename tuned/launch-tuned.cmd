@echo off
rem Split GPU thread on (the default), unsafe preemptive reads on, no profiling.
setlocal
set "EDEN_NX2_UNSAFE_PREEMPTIVE_READS=1"
cd /d "%~dp0"
start "" "eglantine.exe" %*
endlocal
