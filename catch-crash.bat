@echo off
REM ── Capture a backtrace for the marquee/resize crash ────────────────────────
REM Runs RE-MOCT under gdb. Play Zi'tah, drag the border until it crashes.
REM gdb catches the fail-fast and writes a backtrace to the file below.
REM Close the window normally if it does NOT crash - the file still gets written.

set OUT=%USERPROFILE%\Desktop\remoct-backtrace.txt

echo Launching RE-MOCT under gdb...
echo Backtrace will be written to: %OUT%
echo.
echo   1. Play the Zi'tah track
echo   2. Drag the window border back and forth until it crashes
echo.

"C:\msys64\ucrt64\bin\gdb.exe" -batch ^
  -ex "set pagination off" ^
  -ex "set confirm off" ^
  -ex run ^
  -ex "echo \n===== CRASH =====\n" ^
  -ex "bt 60" ^
  -ex "echo \n===== LOCALS =====\n" ^
  -ex "info locals" ^
  -ex "echo \n===== ARGS =====\n" ^
  -ex "info args" ^
  -ex "echo \n===== ALL THREADS =====\n" ^
  -ex "thread apply all bt 25" ^
  --args "%~dp0build\bin\remoct.exe"  > "%OUT%" 2>&1

echo.
echo Done. Wrote %OUT%
pause
